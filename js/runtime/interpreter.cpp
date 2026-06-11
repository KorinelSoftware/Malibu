// js/runtime/interpreter.cpp
// Register-based interpreter for MalibuJS: environments, prototype property
// access, exceptions, and integration with the mark-sweep heap.

#include "malibu/js/runtime/interpreter.h"
#include "malibu/js/bytecode/bytecode.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace malibu::js::runtime {

using bytecode::OpCode;
using bytecode::decode;
using bytecode::Instruction;

namespace {
std::u16string u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string narrow(const std::u16string& s) { std::string r; r.reserve(s.size()); for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
bool parse_index(const std::u16string& s, size_t& out) {
    if (s.empty()) return false;
    size_t v = 0;
    for (char16_t c : s) { if (c < u'0' || c > u'9') return false; v = v * 10 + (c - u'0'); }
    out = v; return true;
}
constexpr int kMaxCallDepth = 2000;
}  // namespace

Interpreter::Interpreter(heap::Heap& heap) : heap_(heap) {
    heap_.set_root_enumerator([this](const heap::Heap::MarkFn& mark) { mark_roots(mark); });
    object_proto_  = heap_.alloc<JSObject>();
    array_proto_   = heap_.alloc<JSArray>();   array_proto_->proto = object_proto_;
    string_proto_  = heap_.alloc<JSObject>();  string_proto_->proto = object_proto_;
    promise_proto_ = heap_.alloc<JSObject>();  promise_proto_->proto = object_proto_;
    map_proto_     = heap_.alloc<JSObject>();   map_proto_->proto = object_proto_;
    set_proto_     = heap_.alloc<JSObject>();   set_proto_->proto = object_proto_;
    function_proto_= heap_.alloc<JSObject>();   function_proto_->proto = object_proto_;
    generator_proto_ = heap_.alloc<JSObject>(); generator_proto_->proto = object_proto_;
    number_proto_  = heap_.alloc<JSObject>();  number_proto_->proto = object_proto_;
    boolean_proto_ = heap_.alloc<JSObject>();  boolean_proto_->proto = object_proto_;
    symbol_proto_  = heap_.alloc<JSObject>();  symbol_proto_->proto = object_proto_;
    array_buffer_proto_ = heap_.alloc<JSObject>(); array_buffer_proto_->proto = object_proto_;
    typed_array_proto_  = heap_.alloc<JSObject>(); typed_array_proto_->proto = object_proto_;
    data_view_proto_    = heap_.alloc<JSObject>(); data_view_proto_->proto = object_proto_;
    global_ = heap_.alloc<Environment>();
    global_->is_function_scope = true;
    // The global scope is backed by a real object so that global `var`/function
    // declarations, bare global lookups, `globalThis`, and top-level `this` all
    // observe the same property set (the sloppy-mode global object).
    global_object_ = heap_.alloc<JSObject>();
    global_object_->proto = object_proto_;
    global_->object_backing = global_object_;
    global_->define(u"%this%", Value::make_heap_ptr(global_object_));  // top-level arrow `this`
    install_builtins();
}

void Interpreter::mark_roots(const heap::Heap::MarkFn& mark) {
    if (global_) mark(Value::make_heap_ptr(global_));
    if (global_object_) mark(Value::make_heap_ptr(global_object_));
    if (object_proto_) mark(Value::make_heap_ptr(object_proto_));
    if (array_proto_) mark(Value::make_heap_ptr(array_proto_));
    if (string_proto_) mark(Value::make_heap_ptr(string_proto_));
    if (promise_proto_) mark(Value::make_heap_ptr(promise_proto_));
    if (map_proto_) mark(Value::make_heap_ptr(map_proto_));
    if (set_proto_) mark(Value::make_heap_ptr(set_proto_));
    if (function_proto_) mark(Value::make_heap_ptr(function_proto_));
    if (generator_proto_) mark(Value::make_heap_ptr(generator_proto_));
    if (number_proto_) mark(Value::make_heap_ptr(number_proto_));
    if (boolean_proto_) mark(Value::make_heap_ptr(boolean_proto_));
    if (symbol_proto_) mark(Value::make_heap_ptr(symbol_proto_));
    if (array_buffer_proto_) mark(Value::make_heap_ptr(array_buffer_proto_));
    if (typed_array_proto_) mark(Value::make_heap_ptr(typed_array_proto_));
    if (data_view_proto_) mark(Value::make_heap_ptr(data_view_proto_));
    for (Value v : temp_roots_) mark(v);
    for (auto& [k, ref] : dom_wrappers_) { (void)k; if (ref) mark(Value::make_heap_ptr(ref)); }
    auto mark_frame = [&](Frame* f) {
        for (Value v : f->regs) mark(v);
        if (f->env) mark(Value::make_heap_ptr(f->env));
        mark(f->this_val);
        mark(f->exc_value);
        mark(f->return_value);
        mark(f->resume_value);
        mark(f->yield_value);
        if (f->async_result) mark(Value::make_heap_ptr(f->async_result));
        if (f->await_promise) mark(Value::make_heap_ptr(f->await_promise));
    };
    for (Frame* f : frame_stack_) mark_frame(f);
    for (auto& sf : suspended_frames_) mark_frame(sf.get());
    for (auto& w : gen_frames_) if (auto f = w.lock()) mark_frame(f.get());
    for (auto& mt : microtasks_) for (Value v : mt.roots) mark(v);
    for (Value v : host_roots_) mark(v);
}

void Interpreter::remove_host_root(Value v) {
    for (auto it = host_roots_.begin(); it != host_roots_.end(); ++it) {
        if (*it == v) { host_roots_.erase(it); return; }
    }
}

// ---- allocation helpers ----
JSString*   Interpreter::new_string(std::u16string s) { return heap_.alloc<JSString>(std::move(s)); }
JSObject*   Interpreter::new_object() { auto* o = heap_.alloc<JSObject>(); o->proto = object_proto_; return o; }
JSArray*    Interpreter::new_array()  { auto* a = heap_.alloc<JSArray>(); a->proto = array_proto_; return a; }
JSMap*      Interpreter::new_map()    { auto* m = heap_.alloc<JSMap>(); m->proto = map_proto_; return m; }
JSSet*      Interpreter::new_set()    { auto* s = heap_.alloc<JSSet>(); s->proto = set_proto_; return s; }
Value       Interpreter::str(const std::string& s) { return Value::make_heap_ptr(new_string(u16(s))); }
JSFunction* Interpreter::new_native(const std::u16string& name, NativeFn fn, uint32_t arity) {
    auto* f = heap_.alloc<JSFunction>();
    f->name = name; f->native = std::move(fn); f->arity = arity;
    return f;
}

Value Interpreter::make_dom_node(malibu::NodeHandle h) {
    if (h.is_null()) return Value::make_null();
    uint64_t key = (static_cast<uint64_t>(h.index) << 32) | h.generation;
    auto it = dom_wrappers_.find(key);
    if (it != dom_wrappers_.end()) return Value::make_heap_ptr(it->second);  // stable identity
    auto* ref = heap_.alloc<vm::DomNodeRef>();
    ref->handle = h;
    dom_wrappers_[key] = ref;
    return Value::make_heap_ptr(ref);
}

[[noreturn]] void Interpreter::throw_error(const std::u16string& kind, const std::u16string& message) {
    JSObject* err = new_object();
    // Link to <Kind>.prototype so `instanceof`, `.constructor`, and the
    // inherited `.name` resolve as the spec requires. Fall back to an own
    // `name` if the constructor is not installed.
    bool linked = false;
    if (Value* ctorv = global_->find(kind);
        ctorv && ctorv->is_heap_ptr() && ctorv->as_heap_ptr()->kind == HeapObject::kJSFunction) {
        Value proto = static_cast<JSFunction*>(ctorv->as_heap_ptr())->get(u"prototype");
        if (proto.is_heap_ptr() && proto.as_heap_ptr()->kind == HeapObject::kJSObject) {
            err->proto = static_cast<JSObject*>(proto.as_heap_ptr());
            linked = true;
        }
    }
    if (!linked) err->set(u"name", str(kind), false);
    err->set(u"message", str(message), false);
    err->set(u"stack", str(kind + u": " + message), false);
    throw ThrowSignal{Value::make_heap_ptr(err)};
}

// ---- conversions ----
std::u16string Interpreter::number_to_string(double d) {
    if (std::isnan(d)) return u"NaN";
    if (std::isinf(d)) return d < 0 ? u"-Infinity" : u"Infinity";
    if (d == std::floor(d) && std::abs(d) < 1e15) {
        long long ll = static_cast<long long>(d);
        return u16(std::to_string(ll));
    }
    std::ostringstream oss;
    oss.precision(15);
    oss << d;
    std::string s = oss.str();
    return u16(s);
}

std::u16string Interpreter::to_string(Value v) {
    if (v.is_int32()) return u16(std::to_string(v.as_int32()));
    if (v.is_double()) return number_to_string(v.as_double());
    if (v.is_bool()) return v.as_bool() ? u"true" : u"false";
    if (v.is_null()) return u"null";
    if (v.is_undefined()) return u"undefined";
    if (v.is_heap_ptr()) {
        HeapObject* o = v.as_heap_ptr();
        switch (o->kind) {
            case HeapObject::kJSString: return static_cast<JSString*>(o)->data;
            case HeapObject::kJSArray: {
                auto* a = static_cast<JSArray*>(o);
                std::u16string out;
                for (size_t i = 0; i < a->elements.size(); ++i) {
                    if (i) out += u",";
                    Value e = a->elements[i];
                    if (!e.is_null() && !e.is_undefined()) out += to_string(e);
                }
                return out;
            }
            case HeapObject::kJSFunction: {
                auto* f = static_cast<JSFunction*>(o);
                return u"function " + f->name + u"() { [code] }";
            }
            case HeapObject::kDomNodeRef: return u"[object Node]";
            default: return u"[object Object]";
        }
    }
    return u"undefined";
}

double Interpreter::to_number(Value v) {
    if (v.is_int32()) return v.as_int32();
    if (v.is_double()) return v.as_double();
    if (v.is_bool()) return v.as_bool() ? 1.0 : 0.0;
    if (v.is_null()) return 0.0;
    if (v.is_undefined()) return std::nan("");
    if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSString) {
        std::u16string& s = static_cast<JSString*>(v.as_heap_ptr())->data;
        std::string narrow_s = narrow(s);
        size_t start = narrow_s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return 0.0;  // whitespace / empty → 0
        try { size_t idx; double d = std::stod(narrow_s, &idx); (void)idx; return d; }
        catch (...) { return std::nan(""); }
    }
    return std::nan("");
}

bool Interpreter::to_bool(Value v) {
    if (v.is_bool()) return v.as_bool();
    if (v.is_int32()) return v.as_int32() != 0;
    if (v.is_double()) { double d = v.as_double(); return d != 0.0 && !std::isnan(d); }
    if (v.is_null() || v.is_undefined()) return false;
    if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSString)
        return !static_cast<JSString*>(v.as_heap_ptr())->data.empty();
    return true;
}

int32_t Interpreter::to_int32(Value v) {
    double d = to_number(v);
    if (std::isnan(d) || std::isinf(d)) return 0;
    return static_cast<int32_t>(static_cast<int64_t>(d));
}

std::u16string Interpreter::js_typeof(Value v) {
    if (v.is_undefined()) return u"undefined";
    if (v.is_null()) return u"object";
    if (v.is_bool()) return u"boolean";
    if (v.is_int32() || v.is_double()) return u"number";
    if (v.is_heap_ptr()) {
        switch (v.as_heap_ptr()->kind) {
            case HeapObject::kJSString: return u"string";
            case HeapObject::kJSFunction: return u"function";
            case HeapObject::kJSProxy: return is_callable(v) ? u"function" : u"object";
            default: return u"object";
        }
    }
    return u"undefined";
}

bool Interpreter::strict_equals(Value a, Value b) {
    bool an = a.is_int32() || a.is_double();
    bool bn = b.is_int32() || b.is_double();
    if (an && bn) return to_number(a) == to_number(b);
    if (a.is_bool() && b.is_bool()) return a.as_bool() == b.as_bool();
    if (a.is_null() && b.is_null()) return true;
    if (a.is_undefined() && b.is_undefined()) return true;
    if (a.is_heap_ptr() && b.is_heap_ptr()) {
        HeapObject *oa = a.as_heap_ptr(), *ob = b.as_heap_ptr();
        if (oa->kind == HeapObject::kJSString && ob->kind == HeapObject::kJSString)
            return static_cast<JSString*>(oa)->data == static_cast<JSString*>(ob)->data;
        return oa == ob;  // reference identity
    }
    return false;
}

bool Interpreter::loose_equals(Value a, Value b) {
    if ((a.is_null() || a.is_undefined()) && (b.is_null() || b.is_undefined())) return true;
    bool an = a.is_int32() || a.is_double(), bn = b.is_int32() || b.is_double();
    bool as = a.is_heap_ptr() && a.as_heap_ptr()->kind == HeapObject::kJSString;
    bool bs = b.is_heap_ptr() && b.as_heap_ptr()->kind == HeapObject::kJSString;
    if ((an || a.is_bool()) && (bn || b.is_bool())) return to_number(a) == to_number(b);
    if (an && bs) return to_number(a) == to_number(b);
    if (as && bn) return to_number(a) == to_number(b);
    if (as && bs) return to_string(a) == to_string(b);
    return strict_equals(a, b);
}

// ---- property access ----
bool Interpreter::is_dom_node(Value v) {
    return v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kDomNodeRef;
}
Value Interpreter::dom_get_prop(Value node, const std::u16string& name) {
    Value out = Value::make_undefined();
    if (dom_get_hook_ && dom_get_hook_(*this, node, name, out)) return out;
    return Value::make_undefined();
}
bool Interpreter::dom_set_prop(Value node, const std::u16string& name, Value v) {
    if (dom_set_hook_) return dom_set_hook_(*this, node, name, v);
    return false;
}

Value Interpreter::object_get(JSObject* obj, const std::u16string& name, Value receiver) {
    Property* p = obj->resolve(name);
    if (!p) return Value::make_undefined();
    if (p->is_accessor) {
        if (p->getter.is_heap_ptr()) { std::vector<Value> none; return call(p->getter, receiver, none); }
        return Value::make_undefined();
    }
    return p->value;
}

void Interpreter::object_set(JSObject* obj, const std::u16string& name, Value v, Value receiver, bool enumerable) {
    if (Property* p = obj->resolve(name); p && p->is_accessor) {
        if (p->setter.is_heap_ptr()) { std::vector<Value> a{v}; call(p->setter, receiver, a); }
        return;  // accessor without a setter: silently ignored (non-strict)
    }
    // Integrity levels (sloppy-mode: silently ignore disallowed writes).
    if (obj->frozen) return;                                  // frozen: no mutation at all
    if (!obj->extensible && !obj->has_own(name)) return;      // non-extensible: no new props
    obj->set(name, v, enumerable);
}

Value Interpreter::get_prop(Value obj, const std::u16string& name) {
    if (obj.is_undefined() || obj.is_null())
        throw_error(u"TypeError", u"Cannot read properties of " + to_string(obj) +
                                      u" (reading '" + name + u"')");
    if (is_dom_node(obj)) return dom_get_prop(obj, name);

    // Primitive number / boolean: route through their (shared) wrapper prototype.
    if (obj.is_double() || obj.is_int32())
        return number_proto_ ? number_proto_->get(name) : Value::make_undefined();
    if (obj.is_bool())
        return boolean_proto_ ? boolean_proto_->get(name) : Value::make_undefined();

    if (obj.is_heap_ptr()) {
        HeapObject* o = obj.as_heap_ptr();
        if (o->kind == HeapObject::kJSProxy) {
            auto* px = static_cast<JSProxy*>(o);
            Value trap = get_prop(px->handler, u"get");
            if (is_callable(trap)) {
                std::vector<Value> a{px->target, str(name), obj};
                return call(trap, px->handler, a);
            }
            return get_prop(px->target, name);
        }
        if (o->kind == HeapObject::kJSString) {
            auto* s = static_cast<JSString*>(o);
            if (name == u"length") return Value::make_int32(static_cast<int32_t>(s->data.size()));
            size_t idx;
            if (parse_index(name, idx)) {
                if (idx < s->data.size()) return str(narrow(std::u16string(1, s->data[idx])));
                return Value::make_undefined();
            }
            return string_proto_->get(name);
        }
        if (o->kind == HeapObject::kJSArray) {
            auto* a = static_cast<JSArray*>(o);
            if (name == u"length") return Value::make_int32(static_cast<int32_t>(a->elements.size()));
            size_t idx;
            if (parse_index(name, idx)) return idx < a->elements.size() ? a->elements[idx] : Value::make_undefined();
            return object_get(a, name, obj);
        }
        if (o->kind == HeapObject::kTypedArray) {
            auto* ta = static_cast<JSTypedArray*>(o);
            if (name == u"length")     return Value::make_int32(static_cast<int32_t>(ta->length));
            if (name == u"byteLength") return Value::make_int32(static_cast<int32_t>(ta->byte_length()));
            if (name == u"byteOffset") return Value::make_int32(static_cast<int32_t>(ta->byte_offset));
            if (name == u"buffer")     return ta->buffer ? Value::make_heap_ptr(ta->buffer) : Value::make_undefined();
            size_t idx;
            if (parse_index(name, idx)) return ta_get_index(ta, idx);
            return object_get(ta, name, obj);
        }
        if (o->kind == HeapObject::kDataView) {
            auto* dv = static_cast<JSDataView*>(o);
            if (name == u"byteLength") return Value::make_int32(static_cast<int32_t>(dv->byte_length));
            if (name == u"byteOffset") return Value::make_int32(static_cast<int32_t>(dv->byte_offset));
            if (name == u"buffer")     return dv->buffer ? Value::make_heap_ptr(dv->buffer) : Value::make_undefined();
            return object_get(dv, name, obj);
        }
        if (o->kind == HeapObject::kArrayBuffer) {
            auto* ab = static_cast<JSArrayBuffer*>(o);
            if (name == u"byteLength") return Value::make_int32(static_cast<int32_t>(ab->detached ? 0 : ab->data.size()));
            return array_buffer_proto_ ? array_buffer_proto_->get(name) : Value::make_undefined();
        }
        if (o->kind == HeapObject::kJSMap) {
            if (name == u"size") return Value::make_int32(static_cast<int32_t>(static_cast<JSMap*>(o)->entries.size()));
            return object_get(static_cast<JSObject*>(o), name, obj);
        }
        if (o->kind == HeapObject::kJSSet) {
            if (name == u"size") return Value::make_int32(static_cast<int32_t>(static_cast<JSSet*>(o)->items.size()));
            return object_get(static_cast<JSObject*>(o), name, obj);
        }
        if (o->kind == HeapObject::kJSObject || o->kind == HeapObject::kJSPromise ||
            o->kind == HeapObject::kJSGenerator)
            return object_get(static_cast<JSObject*>(o), name, obj);
        if (o->kind == HeapObject::kJSFunction) {
            auto* f = static_cast<JSFunction*>(o);
            // Explicit own property (e.g. `prototype`, a `static name(){}`), then
            // the static-base chain (class `extends`), takes precedence over the
            // intrinsic name/length fields.
            for (JSFunction* cur = f; cur; ) {
                if (Property* p = cur->find_own(name)) return p->value;
                Value base = cur->get(u"%staticbase%");
                cur = (base.is_heap_ptr() && base.as_heap_ptr()->kind == HeapObject::kJSFunction)
                          ? static_cast<JSFunction*>(base.as_heap_ptr()) : nullptr;
            }
            if (name == u"name") return str(narrow(f->name));
            if (name == u"length") return Value::make_int32(static_cast<int32_t>(f->arity));
            // Finally, Function.prototype (call / apply / bind / toString).
            if (function_proto_) return function_proto_->get(name);
            return Value::make_undefined();
        }
    }
    return Value::make_undefined();
}

void Interpreter::set_prop(Value obj, const std::u16string& name, Value v, bool enumerable) {
    if (obj.is_undefined() || obj.is_null())
        throw_error(u"TypeError", u"Cannot set properties of " + to_string(obj));
    if (is_dom_node(obj)) { dom_set_prop(obj, name, v); return; }
    if (!obj.is_heap_ptr()) return;
    HeapObject* o = obj.as_heap_ptr();
    if (o->kind == HeapObject::kJSProxy) {
        auto* px = static_cast<JSProxy*>(o);
        Value trap = get_prop(px->handler, u"set");
        if (is_callable(trap)) {
            std::vector<Value> a{px->target, str(name), v, obj};
            call(trap, px->handler, a);
        } else {
            set_prop(px->target, name, v);
        }
        return;
    }
    if (o->kind == HeapObject::kJSArray) {
        auto* a = static_cast<JSArray*>(o);
        if (name == u"length") {
            size_t n = static_cast<size_t>(std::max(0.0, to_number(v)));
            a->elements.resize(n, Value::make_undefined());
            return;
        }
        size_t idx;
        if (parse_index(name, idx)) {
            if (idx >= a->elements.size()) a->elements.resize(idx + 1, Value::make_undefined());
            a->elements[idx] = v;
            return;
        }
        object_set(a, name, v, obj, enumerable);
        return;
    }
    if (o->kind == HeapObject::kTypedArray) {
        auto* ta = static_cast<JSTypedArray*>(o);
        size_t idx;
        if (parse_index(name, idx)) { ta_set_index(ta, idx, v); return; }
        object_set(ta, name, v, obj, enumerable); return;
    }
    if (o->kind == HeapObject::kDataView || o->kind == HeapObject::kArrayBuffer) {
        if (o->kind == HeapObject::kDataView) { object_set(static_cast<JSDataView*>(o), name, v, obj, enumerable); }
        return;
    }
    if (o->kind == HeapObject::kJSObject || o->kind == HeapObject::kJSPromise ||
        o->kind == HeapObject::kJSMap || o->kind == HeapObject::kJSSet ||
        o->kind == HeapObject::kJSGenerator) {
        object_set(static_cast<JSObject*>(o), name, v, obj, enumerable); return;
    }
    if (o->kind == HeapObject::kJSFunction) { static_cast<JSFunction*>(o)->set(name, v); return; }
}

Value Interpreter::get_elem(Value obj, Value key) {
    if (obj.is_heap_ptr() && obj.as_heap_ptr()->kind == HeapObject::kJSArray &&
        (key.is_int32() || key.is_double())) {
        auto* a = static_cast<JSArray*>(obj.as_heap_ptr());
        double d = to_number(key);
        if (d >= 0 && d == std::floor(d)) {
            size_t idx = static_cast<size_t>(d);
            return idx < a->elements.size() ? a->elements[idx] : Value::make_undefined();
        }
    }
    return get_prop(obj, to_string(key));
}

void Interpreter::set_elem(Value obj, Value key, Value v, bool enumerable) {
    if (obj.is_heap_ptr() && obj.as_heap_ptr()->kind == HeapObject::kJSArray &&
        (key.is_int32() || key.is_double())) {
        auto* a = static_cast<JSArray*>(obj.as_heap_ptr());
        double d = to_number(key);
        if (d >= 0 && d == std::floor(d)) {
            size_t idx = static_cast<size_t>(d);
            if (idx >= a->elements.size()) a->elements.resize(idx + 1, Value::make_undefined());
            a->elements[idx] = v;
            return;
        }
    }
    set_prop(obj, to_string(key), v, enumerable);
}

Value Interpreter::array_or_string_length(Value v) {
    if (v.is_heap_ptr()) {
        HeapObject* o = v.as_heap_ptr();
        if (o->kind == HeapObject::kJSArray) return Value::make_int32(static_cast<int32_t>(static_cast<JSArray*>(o)->elements.size()));
        if (o->kind == HeapObject::kJSString) return Value::make_int32(static_cast<int32_t>(static_cast<JSString*>(o)->data.size()));
        if (o->kind == HeapObject::kJSObject) return get_prop(v, u"length");
    }
    return Value::make_int32(0);
}

Value Interpreter::make_iterable(Value v, bool keys) {
    JSArray* result = new_array();
    if (v.is_heap_ptr()) {
        HeapObject* o = v.as_heap_ptr();
        if (o->kind == HeapObject::kJSArray) {
            auto* a = static_cast<JSArray*>(o);
            for (size_t i = 0; i < a->elements.size(); ++i)
                result->elements.push_back(keys ? str(std::to_string(i)) : a->elements[i]);
            if (keys) for (auto& p : a->props) if (p.enumerable) result->elements.push_back(str(narrow(p.key)));
        } else if (o->kind == HeapObject::kJSString) {
            auto* s = static_cast<JSString*>(o);
            for (size_t i = 0; i < s->data.size(); ++i)
                result->elements.push_back(keys ? str(std::to_string(i))
                                                : str(narrow(std::u16string(1, s->data[i]))));
        } else if (o->kind == HeapObject::kJSSet) {
            for (Value v : static_cast<JSSet*>(o)->items) result->elements.push_back(v);
        } else if (o->kind == HeapObject::kJSMap) {
            // for-of over a Map yields [key, value] pairs.
            for (auto& [k, v] : static_cast<JSMap*>(o)->entries) {
                JSArray* pair = new_array();
                pair->elements.push_back(k);
                pair->elements.push_back(v);
                result->elements.push_back(Value::make_heap_ptr(pair));
            }
        } else if (o->kind == HeapObject::kTypedArray) {
            auto* ta = static_cast<JSTypedArray*>(o);
            for (size_t i = 0; i < ta->length; ++i)
                result->elements.push_back(keys ? str(std::to_string(i)) : ta_get_index(ta, i));
        } else if (o->kind == HeapObject::kJSGenerator) {
            drive_iterator(v, result);          // a generator is its own iterator
        } else if (o->kind == HeapObject::kJSObject) {
            auto* obj = static_cast<JSObject*>(o);
            Value itf = obj->get(u"@@iterator");
            bool itf_callable = itf.is_heap_ptr() && itf.as_heap_ptr()->kind == HeapObject::kJSFunction;
            bool self_iter = !itf_callable && [&] { Value n = obj->get(u"next"); return n.is_heap_ptr() && n.as_heap_ptr()->kind == HeapObject::kJSFunction; }();
            if (itf_callable) {
                std::vector<Value> none;
                drive_iterator(call(itf, v, none), result);
            } else if (self_iter) {
                drive_iterator(v, result);
            } else {
                // Plain object: enumerate values (non-spec but pragmatic for for-in-like use).
                for (auto& p : obj->props)
                    if (p.enumerable) result->elements.push_back(keys ? str(narrow(p.key)) : p.value);
            }
        }
    }
    return Value::make_heap_ptr(result);
}

// Drives the iterator protocol (repeated next() until done), collecting values.
void Interpreter::drive_iterator(Value iter, JSArray* out) {
    const int kGuard = 10'000'000;  // cap to avoid hanging on infinite iterators
    for (int i = 0; i < kGuard; ++i) {
        Value nextfn = get_prop(iter, u"next");
        if (!(nextfn.is_heap_ptr() && nextfn.as_heap_ptr()->kind == HeapObject::kJSFunction)) break;
        std::vector<Value> none;
        Value r = call(nextfn, iter, none);
        if (to_bool(get_prop(r, u"done"))) break;
        out->elements.push_back(get_prop(r, u"value"));
    }
}

// ---- calling ----
Value Interpreter::call(Value callee, Value this_val, std::vector<Value>& args) {
    // Proxy [[Call]]: invoke the `apply` trap (target, thisArg, argList) or
    // forward to the target.
    if (callee.is_heap_ptr() && callee.as_heap_ptr()->kind == HeapObject::kJSProxy) {
        auto* px = static_cast<JSProxy*>(callee.as_heap_ptr());
        Value trap = get_prop(px->handler, u"apply");
        if (is_callable(trap)) {
            JSArray* arr = new_array(); arr->elements = args;
            std::vector<Value> ta{px->target, this_val, Value::make_heap_ptr(arr)};
            return call(trap, px->handler, ta);
        }
        return call(px->target, this_val, args);
    }
    if (!callee.is_heap_ptr() || callee.as_heap_ptr()->kind != HeapObject::kJSFunction)
        throw_error(u"TypeError", to_string(callee) + u" is not a function");
    auto* fn = static_cast<JSFunction*>(callee.as_heap_ptr());

    if (++call_depth_ > kMaxCallDepth) { --call_depth_; throw_error(u"RangeError", u"Maximum call stack size exceeded"); }
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } guard{call_depth_};

    if (fn->is_native()) return fn->native(*this, this_val, args);
    if (fn->code && fn->code->is_generator) return make_generator(fn, this_val, args);
    if (fn->code && fn->code->is_async) return call_async(fn, this_val, args);

    Frame frame;
    frame.fn = fn->code;
    frame.this_val = this_val;
    frame.regs.assign(fn->code ? fn->code->num_registers : 0, Value::make_undefined());
    frame.env = heap_.alloc<Environment>();
    frame.env->parent = fn->closure;
    frame.env->is_function_scope = true;
    // Non-arrow functions own `this`; arrows inherit %this% via the closure chain.
    if (!fn->code->is_arrow) frame.env->define(u"%this%", this_val);
    for (size_t i = 0; i < fn->code->param_names.size(); ++i)
        frame.env->define(fn->code->param_names[i], i < args.size() ? args[i] : Value::make_undefined());
    // arguments array
    JSArray* argv = new_array();
    argv->elements = args;
    frame.env->define(u"arguments", Value::make_heap_ptr(argv));

    return run_frame(frame);
}

std::vector<Value> Interpreter::spread_args(Value array) {
    std::vector<Value> out;
    if (array.is_heap_ptr() && array.as_heap_ptr()->kind == HeapObject::kJSArray)
        out = static_cast<JSArray*>(array.as_heap_ptr())->elements;
    return out;
}

std::vector<Value> Interpreter::to_values(Value v) {
    return spread_args(make_iterable(v, false));
}

namespace {
Value clone_rec(Interpreter& in, Value v, std::vector<std::pair<HeapObject*, Value>>& seen);
}

Value Interpreter::deep_clone(Value v) {
    std::vector<std::pair<HeapObject*, Value>> seen;
    return clone_rec(*this, v, seen);
}

namespace {
Value clone_rec(Interpreter& in, Value v, std::vector<std::pair<HeapObject*, Value>>& seen) {
    if (!v.is_heap_ptr()) return v;
    HeapObject* o = v.as_heap_ptr();
    if (o->kind == HeapObject::kJSString || o->kind == HeapObject::kJSFunction) return v;
    for (auto& [k, c] : seen) if (k == o) return c;  // preserve shared/cyclic refs
    switch (o->kind) {
        case HeapObject::kJSArray: {
            auto* a = static_cast<JSArray*>(o); JSArray* c = in.new_array();
            Value cv = Value::make_heap_ptr(c); seen.emplace_back(o, cv);
            for (Value e : a->elements) c->elements.push_back(clone_rec(in, e, seen));
            for (auto& p : a->props) if (p.enumerable && !p.is_accessor) c->set(p.key, clone_rec(in, p.value, seen));
            return cv;
        }
        case HeapObject::kJSMap: {
            auto* m = static_cast<JSMap*>(o); JSMap* c = in.new_map();
            Value cv = Value::make_heap_ptr(c); seen.emplace_back(o, cv);
            for (auto& [k, val] : m->entries) c->entries.emplace_back(clone_rec(in, k, seen), clone_rec(in, val, seen));
            return cv;
        }
        case HeapObject::kJSSet: {
            auto* s = static_cast<JSSet*>(o); JSSet* c = in.new_set();
            Value cv = Value::make_heap_ptr(c); seen.emplace_back(o, cv);
            for (Value e : s->items) c->items.push_back(clone_rec(in, e, seen));
            return cv;
        }
        default: {  // plain object
            auto* obj = static_cast<JSObject*>(o); JSObject* c = in.new_object();
            Value cv = Value::make_heap_ptr(c); seen.emplace_back(o, cv);
            for (auto& p : obj->props) if (p.enumerable && !p.is_accessor) c->set(p.key, clone_rec(in, p.value, seen));
            return cv;
        }
    }
}
}  // namespace

Value Interpreter::construct(Value callee, std::vector<Value>& args) {
    // Proxy [[Construct]]: invoke the `construct` trap (target, argList, newTarget)
    // or forward to the target.
    if (callee.is_heap_ptr() && callee.as_heap_ptr()->kind == HeapObject::kJSProxy) {
        auto* px = static_cast<JSProxy*>(callee.as_heap_ptr());
        Value trap = get_prop(px->handler, u"construct");
        if (is_callable(trap)) {
            JSArray* arr = new_array(); arr->elements = args;
            std::vector<Value> ta{px->target, Value::make_heap_ptr(arr), px->target};
            return call(trap, px->handler, ta);
        }
        return construct(px->target, args);
    }
    if (!callee.is_heap_ptr() || callee.as_heap_ptr()->kind != HeapObject::kJSFunction)
        throw_error(u"TypeError", to_string(callee) + u" is not a constructor");
    auto* fn = static_cast<JSFunction*>(callee.as_heap_ptr());
    JSObject* obj = new_object();
    Value proto = fn->get(u"prototype");
    if (proto.is_heap_ptr() && proto.as_heap_ptr()->kind == HeapObject::kJSObject)
        obj->proto = static_cast<JSObject*>(proto.as_heap_ptr());
    Value this_val = Value::make_heap_ptr(obj);
    Value result = call(callee, this_val, args);
    if (result.is_heap_ptr() && result.as_heap_ptr()->kind != HeapObject::kJSString &&
        result.as_heap_ptr()->kind != HeapObject::kDomNodeRef &&
        result.as_heap_ptr()->kind != HeapObject::kEnvironment)
        return result;  // any object-like return value replaces the fresh `this`
    return this_val;
}

Value Interpreter::run_program(const compiler::Function* program) {
    Frame frame;
    frame.fn = program;
    frame.regs.assign(program->num_registers, Value::make_undefined());
    // Top-level declarations persist in the global scope (REPL-style), so
    // successive evaluate() calls share state.
    frame.env = global_;
    frame.this_val = Value::make_heap_ptr(global_object_);  // sloppy global `this`
    return run_frame(frame);
}

bool Interpreter::unwind_to_handler(Frame& frame, Value exc) {
    while (!frame.handlers.empty()) {
        Handler h = frame.handlers.back();
        frame.handlers.pop_back();
        frame.env = h.saved_env;
        if (h.flags & 1) {  // has catch
            frame.regs[h.exc_reg] = exc;
            frame.pc = static_cast<size_t>(h.catch_pc);
            frame.pending_exc = false;
            return true;
        }
        if (h.flags & 2) {  // finally only
            frame.pending_exc = true;
            frame.exc_value = exc;
            frame.pc = static_cast<size_t>(h.finally_pc);
            return true;
        }
    }
    return false;
}

Value Interpreter::run_frame(Frame& frame) {
    frame_stack_.push_back(&frame);
    struct Pop { std::vector<Frame*>& s; ~Pop() { s.pop_back(); } } pop{frame_stack_};

    const auto& code = frame.fn->code;
    auto& regs = frame.regs;
    auto& f = *frame.fn;

    while (frame.pc < code.size()) {
        if (heap_.should_collect()) heap_.collect();
        try {
            Instruction in = decode(code[frame.pc++]);
            switch (in.op) {
                case OpCode::Nop: break;
                case OpCode::Move: regs[in.dst] = regs[in.src_a]; break;
                case OpCode::LoadConst: regs[in.dst] = f.num_consts[static_cast<uint16_t>(in.imm16)]; break;
                case OpCode::LoadString: regs[in.dst] = str(narrow(f.str_consts[static_cast<uint16_t>(in.imm16)])); break;
                case OpCode::LoadUndefined: regs[in.dst] = Value::make_undefined(); break;
                case OpCode::LoadNull: regs[in.dst] = Value::make_null(); break;
                case OpCode::LoadBool: regs[in.dst] = Value::make_bool(in.imm16 != 0); break;
                case OpCode::LoadThis: regs[in.dst] = frame.this_val; break;

                case OpCode::DefineVar: {
                    const std::u16string& name = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    Environment* target = (in.dst == 1) ? frame.env->function_scope() : frame.env;
                    target->define(name, regs[in.src_a]);
                    break;
                }
                case OpCode::LoadVar: {
                    const std::u16string& name = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    Value* slot = frame.env->find(name);
                    if (!slot) throw_error(u"ReferenceError", name + u" is not defined");
                    regs[in.dst] = *slot;
                    break;
                }
                case OpCode::LoadVarOrUndef: {
                    const std::u16string& name = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    Value* slot = frame.env->find(name);
                    regs[in.dst] = slot ? *slot : Value::make_undefined();  // `typeof x` never throws
                    break;
                }
                case OpCode::StoreVar: {
                    const std::u16string& name = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    if (!frame.env->set(name, regs[in.src_a])) global_->define(name, regs[in.src_a]);
                    break;
                }
                case OpCode::PushScope: {
                    Environment* e = heap_.alloc<Environment>();
                    e->parent = frame.env;
                    frame.env = e;
                    break;
                }
                case OpCode::PopScope: if (frame.env->parent) frame.env = frame.env->parent; break;
                case OpCode::PushWithScope: {
                    Value o = regs[in.src_a];
                    if (o.is_undefined() || o.is_null())
                        throw_error(u"TypeError", u"Cannot convert undefined or null to object");
                    Environment* e = heap_.alloc<Environment>();
                    e->parent = frame.env;
                    if (o.is_heap_ptr() && (o.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                            o.as_heap_ptr()->kind == HeapObject::kJSArray ||
                                            o.as_heap_ptr()->kind == HeapObject::kJSMap ||
                                            o.as_heap_ptr()->kind == HeapObject::kJSSet)) {
                        e->object_backing = static_cast<JSObject*>(o.as_heap_ptr());
                        e->is_with = true;
                    }
                    frame.env = e;
                    break;
                }

                case OpCode::Add: {
                    Value a = regs[in.src_a], b = regs[in.src_b];
                    bool as = a.is_heap_ptr() && a.as_heap_ptr()->kind == HeapObject::kJSString;
                    bool bs = b.is_heap_ptr() && b.as_heap_ptr()->kind == HeapObject::kJSString;
                    bool ao = a.is_heap_ptr() && !as, bo = b.is_heap_ptr() && !bs;
                    if (as || bs || ao || bo) regs[in.dst] = str(narrow(to_string(a) + to_string(b)));
                    else {
                        double r = to_number(a) + to_number(b);
                        regs[in.dst] = (r == std::floor(r) && std::abs(r) < 2147483647.0 && !std::isnan(r))
                                           ? Value::make_int32(static_cast<int32_t>(r)) : Value::make_double(r);
                    }
                    break;
                }
                case OpCode::Sub: regs[in.dst] = Value::make_double(to_number(regs[in.src_a]) - to_number(regs[in.src_b])); break;
                case OpCode::Mul: regs[in.dst] = Value::make_double(to_number(regs[in.src_a]) * to_number(regs[in.src_b])); break;
                case OpCode::Div: regs[in.dst] = Value::make_double(to_number(regs[in.src_a]) / to_number(regs[in.src_b])); break;
                case OpCode::Mod: regs[in.dst] = Value::make_double(std::fmod(to_number(regs[in.src_a]), to_number(regs[in.src_b]))); break;
                case OpCode::Pow: regs[in.dst] = Value::make_double(std::pow(to_number(regs[in.src_a]), to_number(regs[in.src_b]))); break;
                case OpCode::Neg: regs[in.dst] = Value::make_double(-to_number(regs[in.src_a])); break;
                case OpCode::BitNot: regs[in.dst] = Value::make_int32(~to_int32(regs[in.src_a])); break;
                case OpCode::BitAnd: regs[in.dst] = Value::make_int32(to_int32(regs[in.src_a]) & to_int32(regs[in.src_b])); break;
                case OpCode::BitOr:  regs[in.dst] = Value::make_int32(to_int32(regs[in.src_a]) | to_int32(regs[in.src_b])); break;
                case OpCode::BitXor: regs[in.dst] = Value::make_int32(to_int32(regs[in.src_a]) ^ to_int32(regs[in.src_b])); break;
                case OpCode::Shl: regs[in.dst] = Value::make_int32(to_int32(regs[in.src_a]) << (to_int32(regs[in.src_b]) & 31)); break;
                case OpCode::Shr: regs[in.dst] = Value::make_int32(to_int32(regs[in.src_a]) >> (to_int32(regs[in.src_b]) & 31)); break;
                case OpCode::UShr: regs[in.dst] = Value::make_int32(static_cast<int32_t>(static_cast<uint32_t>(to_int32(regs[in.src_a])) >> (to_int32(regs[in.src_b]) & 31))); break;

                case OpCode::Eq:  regs[in.dst] = Value::make_bool(loose_equals(regs[in.src_a], regs[in.src_b])); break;
                case OpCode::NEq: regs[in.dst] = Value::make_bool(!loose_equals(regs[in.src_a], regs[in.src_b])); break;
                case OpCode::StrictEq:  regs[in.dst] = Value::make_bool(strict_equals(regs[in.src_a], regs[in.src_b])); break;
                case OpCode::StrictNEq: regs[in.dst] = Value::make_bool(!strict_equals(regs[in.src_a], regs[in.src_b])); break;
                case OpCode::Lt: case OpCode::Lte: case OpCode::Gt: case OpCode::Gte: {
                    Value a = regs[in.src_a], b = regs[in.src_b];
                    bool as = a.is_heap_ptr() && a.as_heap_ptr()->kind == HeapObject::kJSString;
                    bool bs = b.is_heap_ptr() && b.as_heap_ptr()->kind == HeapObject::kJSString;
                    bool res;
                    if (as && bs) {
                        auto& x = static_cast<JSString*>(a.as_heap_ptr())->data;
                        auto& y = static_cast<JSString*>(b.as_heap_ptr())->data;
                        res = in.op == OpCode::Lt ? x < y : in.op == OpCode::Lte ? x <= y : in.op == OpCode::Gt ? x > y : x >= y;
                    } else {
                        double x = to_number(a), y = to_number(b);
                        res = in.op == OpCode::Lt ? x < y : in.op == OpCode::Lte ? x <= y : in.op == OpCode::Gt ? x > y : x >= y;
                    }
                    regs[in.dst] = Value::make_bool(res);
                    break;
                }
                case OpCode::LogNot: regs[in.dst] = Value::make_bool(!to_bool(regs[in.src_a])); break;
                case OpCode::TypeOf: regs[in.dst] = str(narrow(js_typeof(regs[in.src_a]))); break;
                case OpCode::In: {
                    Value k = regs[in.src_a], o = regs[in.src_b];
                    bool present = false;
                    if (o.is_heap_ptr()) {
                        std::u16string key = to_string(k);
                        HeapObject* obj = o.as_heap_ptr();
                        if (obj->kind == HeapObject::kJSProxy) {
                            auto* px = static_cast<JSProxy*>(obj);
                            Value trap = get_prop(px->handler, u"has");
                            if (is_callable(trap)) {
                                std::vector<Value> a{px->target, str(key)};
                                present = to_bool(call(trap, px->handler, a));
                            } else {
                                std::vector<Value> none;  // forward: key in target
                                Value tv = px->target;
                                if (tv.is_heap_ptr() && (tv.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                                         tv.as_heap_ptr()->kind == HeapObject::kJSArray))
                                    present = static_cast<JSObject*>(tv.as_heap_ptr())->has(key);
                                (void)none;
                            }
                        } else if (obj->kind == HeapObject::kJSObject || obj->kind == HeapObject::kJSArray ||
                                   obj->kind == HeapObject::kTypedArray) {
                            if (obj->kind == HeapObject::kTypedArray) {
                                auto* ta = static_cast<JSTypedArray*>(obj);
                                size_t idx;
                                if (parse_index(key, idx) && idx < ta->length) { present = true; }
                                else present = ta->has(key);
                            } else {
                                present = static_cast<JSObject*>(obj)->has(key);
                            }
                        }
                    }
                    regs[in.dst] = Value::make_bool(present);
                    break;
                }
                case OpCode::InstanceOf: {
                    Value o = regs[in.src_a], ctor = regs[in.src_b];
                    bool res = false;
                    if (o.is_heap_ptr() && ctor.is_heap_ptr() && ctor.as_heap_ptr()->kind == HeapObject::kJSFunction) {
                        Value proto = static_cast<JSFunction*>(ctor.as_heap_ptr())->get(u"prototype");
                        HeapObject::Kind k = o.as_heap_ptr()->kind;
                        // Every JSObject-derived kind carries a proto chain.
                        if (k == HeapObject::kJSObject || k == HeapObject::kJSArray ||
                            k == HeapObject::kJSPromise || k == HeapObject::kJSMap ||
                            k == HeapObject::kJSSet || k == HeapObject::kJSGenerator ||
                            k == HeapObject::kTypedArray || k == HeapObject::kDataView ||
                            k == HeapObject::kArrayBuffer) {
                            JSObject* p = static_cast<JSObject*>(o.as_heap_ptr())->proto;
                            while (p) { if (Value::make_heap_ptr(p) == proto) { res = true; break; } p = p->proto; }
                        }
                    }
                    regs[in.dst] = Value::make_bool(res);
                    break;
                }

                case OpCode::Jump: frame.pc = static_cast<uint16_t>(in.imm16); break;
                case OpCode::JumpIfTrue: if (to_bool(regs[in.src_a])) frame.pc = static_cast<uint16_t>(in.imm16); break;
                case OpCode::JumpIfFalse: if (!to_bool(regs[in.src_a])) frame.pc = static_cast<uint16_t>(in.imm16); break;

                case OpCode::NewObject: regs[in.dst] = Value::make_heap_ptr(new_object()); break;
                case OpCode::NewArray: {
                    JSArray* a = new_array();
                    int count = static_cast<uint16_t>(in.imm16);
                    for (int i = 0; i < count; ++i) a->elements.push_back(regs[in.src_a + i]);
                    regs[in.dst] = Value::make_heap_ptr(a);
                    break;
                }
                case OpCode::NewClosure: {
                    const auto& tmpl = f.functions[static_cast<uint16_t>(in.imm16)];
                    JSFunction* fn = heap_.alloc<JSFunction>();
                    fn->name = tmpl->name; fn->arity = tmpl->arity;
                    fn->code = tmpl.get(); fn->closure = frame.env;
                    // every function gets a fresh .prototype object
                    JSObject* proto = new_object();
                    proto->set(u"constructor", Value::make_heap_ptr(fn), false);
                    fn->set(u"prototype", Value::make_heap_ptr(proto));
                    regs[in.dst] = Value::make_heap_ptr(fn);
                    break;
                }

                case OpCode::GetProp: regs[in.dst] = get_prop(regs[in.src_a], f.str_consts[static_cast<uint16_t>(in.imm16)]); break;
                case OpCode::SetProp: set_prop(regs[in.dst], f.str_consts[static_cast<uint16_t>(in.imm16)], regs[in.src_a], in.src_b == 0); break;
                case OpCode::GetElem: regs[in.dst] = get_elem(regs[in.src_a], regs[in.src_b]); break;
                case OpCode::SetElem: set_elem(regs[in.dst], regs[in.src_a], regs[in.src_b], in.imm16 == 0); break;
                case OpCode::GetLength: regs[in.dst] = array_or_string_length(regs[in.src_a]); break;
                case OpCode::ToIterable: regs[in.dst] = make_iterable(regs[in.src_a], in.imm16 != 0); break;

                case OpCode::Call: {
                    int base = in.src_a, argc = static_cast<uint16_t>(in.imm16);
                    Value callee = regs[base], this_val = regs[base + 1];
                    std::vector<Value> args(regs.begin() + base + 2, regs.begin() + base + 2 + argc);
                    regs[in.dst] = call(callee, this_val, args);
                    break;
                }
                case OpCode::Construct: {
                    int base = in.src_a, argc = static_cast<uint16_t>(in.imm16);
                    Value callee = regs[base];
                    std::vector<Value> args(regs.begin() + base + 1, regs.begin() + base + 1 + argc);
                    regs[in.dst] = construct(callee, args);
                    break;
                }
                case OpCode::SetProto: {
                    // Wire an internal [[Prototype]]: for objects this is the
                    // proto link (instance method inheritance); for functions we
                    // record a hidden static-base so `Child.staticFromParent`
                    // resolves up the constructor chain (class `extends`).
                    Value target = regs[in.dst], proto = regs[in.src_a];
                    if (target.is_heap_ptr()) {
                        HeapObject* t = target.as_heap_ptr();
                        if (t->kind == HeapObject::kJSObject || t->kind == HeapObject::kJSArray ||
                            t->kind == HeapObject::kJSPromise) {
                            auto* o = static_cast<JSObject*>(t);
                            // `class Sub extends Base`: the proto value is the Base
                            // *constructor*; the instance proto chain must link to
                            // Base.prototype, so deref a function to its .prototype.
                            JSObject* pp = nullptr;
                            if (proto.is_heap_ptr()) {
                                HeapObject* ph = proto.as_heap_ptr();
                                if (ph->kind == HeapObject::kJSObject || ph->kind == HeapObject::kJSArray)
                                    pp = static_cast<JSObject*>(ph);
                                else if (ph->kind == HeapObject::kJSFunction) {
                                    Value pr = static_cast<JSFunction*>(ph)->get(u"prototype");
                                    if (pr.is_heap_ptr() && (pr.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                                             pr.as_heap_ptr()->kind == HeapObject::kJSArray))
                                        pp = static_cast<JSObject*>(pr.as_heap_ptr());
                                }
                            }
                            o->proto = pp;
                        } else if (t->kind == HeapObject::kJSFunction) {
                            static_cast<JSFunction*>(t)->set(u"%staticbase%", proto);
                        }
                    }
                    break;
                }
                case OpCode::CallV: {
                    Value callee = regs[in.src_a], this_val = regs[in.src_b];
                    std::vector<Value> args = spread_args(regs[static_cast<uint16_t>(in.imm16)]);
                    regs[in.dst] = call(callee, this_val, args);
                    break;
                }
                case OpCode::ConstructV: {
                    Value callee = regs[in.src_a];
                    std::vector<Value> args = spread_args(regs[static_cast<uint16_t>(in.imm16)]);
                    regs[in.dst] = construct(callee, args);
                    break;
                }
                case OpCode::ArrayAppend: {
                    Value arrv = regs[in.dst];
                    if (arrv.is_heap_ptr() && arrv.as_heap_ptr()->kind == HeapObject::kJSArray) {
                        auto* arr = static_cast<JSArray*>(arrv.as_heap_ptr());
                        if (in.imm16 == 0) {
                            arr->elements.push_back(regs[in.src_a]);
                        } else {
                            for (Value e : spread_args(make_iterable(regs[in.src_a], false)))
                                arr->elements.push_back(e);
                        }
                    }
                    break;
                }
                case OpCode::DefineAccessor: {
                    Value target = regs[in.dst], fnv = regs[in.src_a];
                    const std::u16string& nm = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    if (target.is_heap_ptr()) {
                        HeapObject* t = target.as_heap_ptr();
                        if (t->kind == HeapObject::kJSObject || t->kind == HeapObject::kJSArray)
                            static_cast<JSObject*>(t)->define_accessor(nm, fnv, in.src_b != 0);
                        else if (t->kind == HeapObject::kJSFunction)
                            static_cast<JSFunction*>(t)->set(nm, fnv);  // static accessor: degraded to data
                    }
                    break;
                }
                case OpCode::DefineAccessorV: {
                    Value target = regs[in.dst], fnv = regs[in.src_a];
                    std::u16string nm = to_string(regs[in.src_b]);
                    if (target.is_heap_ptr()) {
                        HeapObject* t = target.as_heap_ptr();
                        if (t->kind == HeapObject::kJSObject || t->kind == HeapObject::kJSArray)
                            static_cast<JSObject*>(t)->define_accessor(nm, fnv, in.imm16 != 0);
                        else if (t->kind == HeapObject::kJSFunction)
                            static_cast<JSFunction*>(t)->set(nm, fnv);
                    }
                    break;
                }
                case OpCode::DeleteProp:
                case OpCode::DeleteElem: {
                    Value obj = regs[in.src_a];
                    std::u16string key = (in.op == OpCode::DeleteProp)
                        ? f.str_consts[static_cast<uint16_t>(in.imm16)]
                        : to_string(regs[in.src_b]);
                    bool ok = true;
                    if (obj.is_heap_ptr()) {
                        HeapObject* o = obj.as_heap_ptr();
                        if (o->kind == HeapObject::kJSProxy) {
                            auto* px = static_cast<JSProxy*>(o);
                            Value trap = get_prop(px->handler, u"deleteProperty");
                            if (is_callable(trap)) {
                                std::vector<Value> a{px->target, str(key)};
                                ok = to_bool(call(trap, px->handler, a));
                            } else if (px->target.is_heap_ptr() &&
                                       (px->target.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                        px->target.as_heap_ptr()->kind == HeapObject::kJSArray)) {
                                static_cast<JSObject*>(px->target.as_heap_ptr())->delete_prop(key);
                            }
                            regs[in.dst] = Value::make_bool(ok);
                            break;
                        }
                        if (o->kind == HeapObject::kJSArray) {
                            auto* a = static_cast<JSArray*>(o);
                            size_t idx;
                            if (parse_index(key, idx)) {
                                if (idx < a->elements.size()) a->elements[idx] = Value::make_undefined();
                            } else {
                                a->delete_prop(key);
                            }
                        } else if (o->kind == HeapObject::kJSObject || o->kind == HeapObject::kJSPromise ||
                                   o->kind == HeapObject::kJSMap || o->kind == HeapObject::kJSSet ||
                                   o->kind == HeapObject::kJSGenerator) {
                            static_cast<JSObject*>(o)->delete_prop(key);
                        }
                    }
                    regs[in.dst] = Value::make_bool(ok);
                    break;
                }
                case OpCode::SetFnName: {
                    Value fv = regs[in.dst];
                    if (fv.is_heap_ptr() && fv.as_heap_ptr()->kind == HeapObject::kJSFunction) {
                        auto* fn = static_cast<JSFunction*>(fv.as_heap_ptr());
                        if (fn->name.empty()) fn->name = f.str_consts[static_cast<uint16_t>(in.imm16)];
                    }
                    break;
                }
                case OpCode::CopyProps: {
                    Value dstv = regs[in.dst], srcv = regs[in.src_a];
                    if (dstv.is_heap_ptr() && dstv.as_heap_ptr()->kind == HeapObject::kJSObject &&
                        srcv.is_heap_ptr() && (srcv.as_heap_ptr()->kind == HeapObject::kJSObject ||
                                               srcv.as_heap_ptr()->kind == HeapObject::kJSArray)) {
                        auto* dst = static_cast<JSObject*>(dstv.as_heap_ptr());
                        auto* src = static_cast<JSObject*>(srcv.as_heap_ptr());
                        for (auto& p : src->props) if (p.enumerable) dst->set(p.key, p.value);
                    }
                    break;
                }

                case OpCode::Return: frame.return_value = regs[in.src_a]; frame.returning = true; break;
                case OpCode::ReturnUndefined: frame.return_value = Value::make_undefined(); frame.returning = true; break;
                case OpCode::Throw: throw ThrowSignal{regs[in.src_a]};

                case OpCode::Await: {
                    if (frame.has_resume_value) {
                        frame.has_resume_value = false;
                        Value v = frame.resume_value;
                        if (frame.resume_is_throw) { frame.resume_is_throw = false; throw ThrowSignal{v}; }
                        regs[in.dst] = v;
                        break;
                    }
                    // First execution: suspend. Re-enter this instruction on resume.
                    frame.await_promise = promise_resolve_value(regs[in.src_a]);
                    frame.pc -= 1;
                    throw SuspendSignal{};
                }
                case OpCode::Yield: {
                    if (frame.has_resume_value) {
                        frame.has_resume_value = false;
                        Value v = frame.resume_value;
                        if (frame.resume_is_throw) { frame.resume_is_throw = false; throw ThrowSignal{v}; }
                        regs[in.dst] = v;   // value of the yield expression = next(v) arg
                        break;
                    }
                    frame.yield_value = regs[in.src_a];
                    frame.pc -= 1;          // re-enter this instruction on resume
                    throw YieldSignal{};
                }

                case OpCode::PushHandler: {
                    Handler h;
                    h.flags = in.dst;
                    h.finally_pc = static_cast<uint16_t>(in.src_a);
                    h.exc_reg = static_cast<uint8_t>(in.src_b);
                    h.catch_pc = static_cast<uint16_t>(in.imm16);
                    h.saved_env = frame.env;
                    frame.handlers.push_back(h);
                    break;
                }
                case OpCode::PopHandler: if (!frame.handlers.empty()) frame.handlers.pop_back(); break;
                case OpCode::EndFinally:
                    if (frame.pending_exc) { frame.pending_exc = false; throw ThrowSignal{frame.exc_value}; }
                    break;

                default:
                    MALIBU_LOG(WARNING, "vm", std::string("unhandled opcode ") + std::to_string(static_cast<int>(in.op)));
                    break;
            }
        } catch (ThrowSignal& sig) {
            if (!unwind_to_handler(frame, sig.value)) throw;  // propagate to caller
        }

        if (frame.returning) return frame.return_value;
    }
    return Value::make_undefined();
}

// ---------------------------------------------------------------------------
// Promises + microtasks
// ---------------------------------------------------------------------------
void Interpreter::enqueue_microtask(MicrotaskFn fn, std::vector<Value> roots) {
    microtasks_.push_back(Microtask{std::move(fn), std::move(roots)});
}

void Interpreter::run_microtasks() {
    while (!microtasks_.empty()) {
        Microtask mt = std::move(microtasks_.front());
        microtasks_.pop_front();
        size_t base = temp_roots_.size();
        for (Value v : mt.roots) temp_roots_.push_back(v);
        if (mt.run) mt.run();
        temp_roots_.resize(base);
    }
}

bool Interpreter::is_callable(Value v) const {
    if (!v.is_heap_ptr()) return false;
    HeapObject* o = v.as_heap_ptr();
    if (o->kind == HeapObject::kJSFunction) return true;
    if (o->kind == HeapObject::kJSProxy) return is_callable(static_cast<JSProxy*>(o)->target);
    return false;
}
bool Interpreter::is_promise(Value v) const {
    return v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kJSPromise;
}

JSPromise* Interpreter::new_promise() {
    auto* p = heap_.alloc<JSPromise>();
    p->proto = promise_proto_;
    return p;
}

void Interpreter::settle_promise(JSPromise* p, JSPromise::State state, Value value) {
    if (p->state != JSPromise::State::Pending) return;
    p->state = state;
    p->result = value;
    std::vector<PromiseReaction> reactions;
    reactions.swap(p->reactions);
    bool fulfilled = (state == JSPromise::State::Fulfilled);
    for (const PromiseReaction& r : reactions) {
        PromiseReaction reaction = r;
        std::vector<Value> roots = {value, reaction.on_fulfilled, reaction.on_rejected};
        if (reaction.result) roots.push_back(Value::make_heap_ptr(reaction.result));
        enqueue_microtask([this, reaction, fulfilled, value]() {
            Value handler = fulfilled ? reaction.on_fulfilled : reaction.on_rejected;
            if (!is_callable(handler)) {
                if (!reaction.result) return;
                if (fulfilled) resolve_promise(reaction.result, value);
                else reject_promise(reaction.result, value);
                return;
            }
            try {
                std::vector<Value> args{value};
                Value x = call(handler, Value::make_undefined(), args);
                if (reaction.result) resolve_promise(reaction.result, x);
            } catch (ThrowSignal& sig) {
                if (reaction.result) reject_promise(reaction.result, sig.value);
            }
        }, std::move(roots));
    }
}

void Interpreter::resolve_promise(JSPromise* p, Value value) {
    if (p->state != JSPromise::State::Pending) return;
    if (is_promise(value) && value.as_heap_ptr() == static_cast<HeapObject*>(p)) {
        reject_promise(p, str(std::string("TypeError: chaining cycle detected")));
        return;
    }
    // Thenable assimilation (also handles our own promises via their .then).
    if (value.is_heap_ptr()) {
        HeapObject* o = value.as_heap_ptr();
        if (o->kind == HeapObject::kJSObject || o->kind == HeapObject::kJSPromise ||
            o->kind == HeapObject::kJSArray) {
            Value then = get_prop(value, u"then");
            if (is_callable(then)) {
                JSPromise* self = p;
                JSFunction* on_f = new_native(u"", [self](Interpreter& in, Value, std::vector<Value>& a) {
                    in.resolve_promise(self, a.empty() ? Value::make_undefined() : a[0]);
                    return Value::make_undefined();
                });
                JSFunction* on_r = new_native(u"", [self](Interpreter& in, Value, std::vector<Value>& a) {
                    in.reject_promise(self, a.empty() ? Value::make_undefined() : a[0]);
                    return Value::make_undefined();
                });
                std::vector<Value> roots{value, then, Value::make_heap_ptr(on_f),
                                         Value::make_heap_ptr(on_r), Value::make_heap_ptr(p)};
                enqueue_microtask([this, value, then, on_f, on_r]() {
                    try {
                        std::vector<Value> args{Value::make_heap_ptr(on_f), Value::make_heap_ptr(on_r)};
                        call(then, value, args);
                    } catch (ThrowSignal&) {}
                }, std::move(roots));
                return;
            }
        }
    }
    settle_promise(p, JSPromise::State::Fulfilled, value);
}

void Interpreter::reject_promise(JSPromise* p, Value reason) {
    settle_promise(p, JSPromise::State::Rejected, reason);
}

Value Interpreter::promise_then(JSPromise* p, Value on_fulfilled, Value on_rejected) {
    JSPromise* result = new_promise();
    PromiseReaction reaction{on_fulfilled, on_rejected, result};
    if (p->state == JSPromise::State::Pending) {
        p->reactions.push_back(reaction);
    } else {
        bool fulfilled = (p->state == JSPromise::State::Fulfilled);
        Value value = p->result;
        std::vector<Value> roots{value, on_fulfilled, on_rejected, Value::make_heap_ptr(result)};
        enqueue_microtask([this, reaction, fulfilled, value]() {
            Value handler = fulfilled ? reaction.on_fulfilled : reaction.on_rejected;
            if (!is_callable(handler)) {
                if (fulfilled) resolve_promise(reaction.result, value);
                else reject_promise(reaction.result, value);
                return;
            }
            try {
                std::vector<Value> args{value};
                Value x = call(handler, Value::make_undefined(), args);
                resolve_promise(reaction.result, x);
            } catch (ThrowSignal& sig) {
                reject_promise(reaction.result, sig.value);
            }
        }, std::move(roots));
    }
    return Value::make_heap_ptr(result);
}

JSPromise* Interpreter::promise_resolve_value(Value v) {
    if (is_promise(v)) return static_cast<JSPromise*>(v.as_heap_ptr());
    JSPromise* p = new_promise();
    resolve_promise(p, v);
    return p;
}

Value Interpreter::await_settled(Value v) {
    if (!is_promise(v)) return v;
    run_microtasks();
    auto* p = static_cast<JSPromise*>(v.as_heap_ptr());
    if (p->state == JSPromise::State::Fulfilled) return p->result;
    if (p->state == JSPromise::State::Rejected) throw ThrowSignal{p->result};
    return Value::make_undefined();  // still pending (needs the event loop)
}

// ---------------------------------------------------------------------------
// async/await — resumable frames
// ---------------------------------------------------------------------------
Value Interpreter::call_async(JSFunction* fn, Value this_val, std::vector<Value>& args) {
    auto sf = std::make_shared<Frame>();
    sf->fn = fn->code;
    sf->is_async = true;
    sf->this_val = this_val;
    sf->regs.assign(fn->code ? fn->code->num_registers : 0, Value::make_undefined());
    sf->env = heap_.alloc<Environment>();
    sf->env->parent = fn->closure;
    sf->env->is_function_scope = true;
    if (!fn->code->is_arrow) sf->env->define(u"%this%", this_val);
    for (size_t i = 0; i < fn->code->param_names.size(); ++i)
        sf->env->define(fn->code->param_names[i], i < args.size() ? args[i] : Value::make_undefined());
    JSArray* argv = new_array();
    argv->elements = args;
    sf->env->define(u"arguments", Value::make_heap_ptr(argv));
    sf->async_result = new_promise();

    suspended_frames_.push_back(sf);
    Value result = Value::make_heap_ptr(sf->async_result);
    drive_async(sf);
    return result;
}

void Interpreter::drive_async(std::shared_ptr<Frame> sf) {
    try {
        Value rv = run_frame(*sf);
        resolve_promise(sf->async_result, rv);
    } catch (SuspendSignal&) {
        JSPromise* p = sf->await_promise;
        sf->await_promise = nullptr;
        std::weak_ptr<Frame> weak = sf;
        JSFunction* on_f = new_native(u"", [this, weak](Interpreter&, Value, std::vector<Value>& a) {
            if (auto s = weak.lock()) resume_async(s, a.empty() ? Value::make_undefined() : a[0], false);
            return Value::make_undefined();
        });
        JSFunction* on_r = new_native(u"", [this, weak](Interpreter&, Value, std::vector<Value>& a) {
            if (auto s = weak.lock()) resume_async(s, a.empty() ? Value::make_undefined() : a[0], true);
            return Value::make_undefined();
        });
        promise_then(p, Value::make_heap_ptr(on_f), Value::make_heap_ptr(on_r));
        return;  // frame remains rooted in suspended_frames_
    } catch (ThrowSignal& sig) {
        reject_promise(sf->async_result, sig.value);
    }
    for (auto it = suspended_frames_.begin(); it != suspended_frames_.end(); ++it) {
        if (it->get() == sf.get()) { suspended_frames_.erase(it); break; }
    }
}

void Interpreter::resume_async(std::shared_ptr<Frame> sf, Value value, bool is_throw) {
    sf->has_resume_value = true;
    sf->resume_value = value;
    sf->resume_is_throw = is_throw;
    drive_async(sf);
}

Value Interpreter::make_generator(JSFunction* fn, Value this_val, std::vector<Value>& args) {
    auto sf = std::make_shared<Frame>();
    sf->fn = fn->code;
    sf->is_generator = true;
    sf->this_val = this_val;
    sf->regs.assign(fn->code ? fn->code->num_registers : 0, Value::make_undefined());
    sf->env = heap_.alloc<Environment>();
    sf->env->parent = fn->closure;
    sf->env->is_function_scope = true;
    if (!fn->code->is_arrow) sf->env->define(u"%this%", this_val);
    for (size_t i = 0; i < fn->code->param_names.size(); ++i)
        sf->env->define(fn->code->param_names[i], i < args.size() ? args[i] : Value::make_undefined());
    JSArray* argv = new_array();
    argv->elements = args;
    sf->env->define(u"arguments", Value::make_heap_ptr(argv));

    auto* gen = heap_.alloc<JSGenerator>();
    gen->proto = generator_proto_;
    gen->frame = sf;                 // shared_ptr<Frame> -> shared_ptr<void>
    gen_frames_.push_back(sf);       // GC roots the frame while it (weakly) lives
    // Opportunistically drop expired weak refs.
    if (gen_frames_.size() > 64)
        gen_frames_.erase(std::remove_if(gen_frames_.begin(), gen_frames_.end(),
                          [](const std::weak_ptr<Frame>& w) { return w.expired(); }), gen_frames_.end());
    return Value::make_heap_ptr(gen);
}

Value Interpreter::gen_resume(JSGenerator* gen, Value value, bool is_throw, bool is_return) {
    auto iter_result = [&](Value v, bool done) {
        JSObject* r = new_object();
        r->set(u"value", v);
        r->set(u"done", Value::make_bool(done));
        return Value::make_heap_ptr(r);
    };
    if (gen->done || !gen->frame) {
        if (is_throw) throw ThrowSignal{value};
        return iter_result(is_return ? value : Value::make_undefined(), true);
    }
    auto sf = std::static_pointer_cast<Frame>(gen->frame);
    if (gen->running) throw_error(u"TypeError", u"Generator is already running");

    if (is_return) { gen->done = true; gen->frame.reset(); return iter_result(value, true); }

    if (sf->started) {
        sf->has_resume_value = true;
        sf->resume_value = value;
        sf->resume_is_throw = is_throw;
    } else {
        sf->started = true;
        if (is_throw) { gen->done = true; gen->frame.reset(); throw ThrowSignal{value}; }
    }

    gen->running = true;
    try {
        Value rv = run_frame(*sf);
        gen->done = true; gen->running = false; gen->frame.reset();
        return iter_result(rv, true);
    } catch (YieldSignal&) {
        gen->running = false;
        return iter_result(sf->yield_value, false);
    } catch (ThrowSignal&) {
        gen->done = true; gen->running = false; gen->frame.reset();
        throw;
    }
}

} // namespace malibu::js::runtime
