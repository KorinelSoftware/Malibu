// js/runtime/typed_arrays.cpp
// ECMAScript binary data: ArrayBuffer, the nine numeric TypedArray views
// (Int8/Uint8/Uint8Clamped/Int16/Uint16/Int32/Uint32/Float32/Float64), and
// DataView. Element access goes through Interpreter::ta_get_index/ta_set_index
// (called from get_prop/set_prop in interpreter.cpp). This is the shared binary
// substrate used by crypto, text codecs, WebAssembly memory, and WebGL buffers.
//
// BigInt64Array/BigUint64Array are intentionally omitted until a real BigInt
// type exists (a Number-backed stand-in would silently lose precision).

#include "malibu/js/runtime/interpreter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace malibu::js::runtime {
namespace {
std::u16string u16(const std::string& s) { return std::u16string(s.begin(), s.end()); }
std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
Value arg(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value::make_undefined(); }

struct TADesc { const char* name; TAKind kind; };
const TADesc kTypes[] = {
    {"Int8Array",         TAKind::Int8},
    {"Uint8Array",        TAKind::Uint8},
    {"Uint8ClampedArray", TAKind::Uint8Clamped},
    {"Int16Array",        TAKind::Int16},
    {"Uint16Array",       TAKind::Uint16},
    {"Int32Array",        TAKind::Int32},
    {"Uint32Array",       TAKind::Uint32},
    {"Float32Array",      TAKind::Float32},
    {"Float64Array",      TAKind::Float64},
};

// ToIndex/ToInteger-style coercion (NaN/inf -> 0, truncate toward zero).
double to_integer(double d) {
    if (std::isnan(d)) return 0;
    if (std::isinf(d)) return d;
    return std::trunc(d);
}
uint8_t clamp_u8(double d) {
    if (std::isnan(d)) return 0;
    if (d <= 0) return 0;
    if (d >= 255) return 255;
    double r = std::nearbyint(d);  // round-half-to-even
    return static_cast<uint8_t>(r);
}
}  // namespace

// ---- element read/write (byte-level, native little-endian) ----
Value Interpreter::ta_get_index(JSTypedArray* ta, size_t idx) {
    if (!ta->buffer || ta->buffer->detached || idx >= ta->length) return Value::make_undefined();
    size_t es = JSTypedArray::elem_size(ta->ta_kind);
    const uint8_t* p = ta->buffer->data.data() + ta->byte_offset + idx * es;
    switch (ta->ta_kind) {
        case TAKind::Int8:   { int8_t v;   std::memcpy(&v, p, 1); return Value::make_int32(v); }
        case TAKind::Uint8:
        case TAKind::Uint8Clamped: { uint8_t v; std::memcpy(&v, p, 1); return Value::make_int32(v); }
        case TAKind::Int16:  { int16_t v;  std::memcpy(&v, p, 2); return Value::make_int32(v); }
        case TAKind::Uint16: { uint16_t v; std::memcpy(&v, p, 2); return Value::make_int32(v); }
        case TAKind::Int32:  { int32_t v;  std::memcpy(&v, p, 4); return Value::make_int32(v); }
        case TAKind::Uint32: { uint32_t v; std::memcpy(&v, p, 4); return Value::make_double(static_cast<double>(v)); }
        case TAKind::Float32:{ float v;    std::memcpy(&v, p, 4); return Value::make_double(static_cast<double>(v)); }
        case TAKind::Float64:{ double v;   std::memcpy(&v, p, 8); return Value::make_double(v); }
    }
    return Value::make_undefined();
}

void Interpreter::ta_set_index(JSTypedArray* ta, size_t idx, Value v) {
    double d = to_number(v);  // may run valueOf; spec evaluates ToNumber even when idx is OOB
    if (!ta->buffer || ta->buffer->detached || idx >= ta->length) return;
    size_t es = JSTypedArray::elem_size(ta->ta_kind);
    uint8_t* p = ta->buffer->data.data() + ta->byte_offset + idx * es;
    switch (ta->ta_kind) {
        case TAKind::Int8:   { int8_t   x = static_cast<int8_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 1); break; }
        case TAKind::Uint8:  { uint8_t  x = static_cast<uint8_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 1); break; }
        case TAKind::Uint8Clamped: { uint8_t x = clamp_u8(d); std::memcpy(p, &x, 1); break; }
        case TAKind::Int16:  { int16_t  x = static_cast<int16_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 2); break; }
        case TAKind::Uint16: { uint16_t x = static_cast<uint16_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 2); break; }
        case TAKind::Int32:  { int32_t  x = static_cast<int32_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 4); break; }
        case TAKind::Uint32: { uint32_t x = static_cast<uint32_t>(static_cast<int64_t>(to_integer(d))); std::memcpy(p, &x, 4); break; }
        case TAKind::Float32:{ float    x = static_cast<float>(d); std::memcpy(p, &x, 4); break; }
        case TAKind::Float64:{ double   x = d; std::memcpy(p, &x, 8); break; }
    }
}

void Interpreter::install_typed_arrays() {
    auto self = this;
    auto new_buffer = [self](size_t len) -> JSArrayBuffer* {
        auto* ab = self->heap_.alloc<JSArrayBuffer>();
        ab->proto = self->array_buffer_proto_;
        ab->data.assign(len, 0);
        return ab;
    };

    // ---- ArrayBuffer ----
    {
        JSFunction* AB = new_native(u"ArrayBuffer", [new_buffer](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            double len = a.empty() ? 0 : in.to_number(arg(a, 0));
            if (std::isnan(len) || len < 0) in.throw_error(u"RangeError", u"Invalid ArrayBuffer length");
            return Value::make_heap_ptr(new_buffer(static_cast<size_t>(len)));
        }, 1);
        AB->set(u"prototype", Value::make_heap_ptr(array_buffer_proto_), false);
        array_buffer_proto_->set(u"constructor", Value::make_heap_ptr(AB), false);
        AB->set(u"isView", Value::make_heap_ptr(new_native(u"isView", [](Interpreter&, Value, std::vector<Value>& a) {
            Value v = arg(a, 0);
            return Value::make_bool(v.is_heap_ptr() && (v.as_heap_ptr()->kind == HeapObject::kTypedArray ||
                                                        v.as_heap_ptr()->kind == HeapObject::kDataView));
        }, 1)));
        array_buffer_proto_->set(u"slice", Value::make_heap_ptr(new_native(u"slice", [new_buffer](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
            if (!t.is_heap_ptr() || t.as_heap_ptr()->kind != HeapObject::kArrayBuffer)
                in.throw_error(u"TypeError", u"ArrayBuffer.prototype.slice called on non-ArrayBuffer");
            auto* ab = static_cast<JSArrayBuffer*>(t.as_heap_ptr());
            long n = static_cast<long>(ab->data.size());
            long begin = a.size() > 0 ? static_cast<long>(in.to_number(a[0])) : 0;
            long end = (a.size() > 1 && !a[1].is_undefined()) ? static_cast<long>(in.to_number(a[1])) : n;
            if (begin < 0) begin += n;
            if (end < 0) end += n;
            begin = std::max(0L, std::min(begin, n));
            end = std::max(0L, std::min(end, n));
            JSArrayBuffer* out = new_buffer(static_cast<size_t>(std::max(0L, end - begin)));
            for (long i = begin; i < end; ++i) out->data[i - begin] = ab->data[i];
            return Value::make_heap_ptr(out);
        }, 2)));
        global_->define(u"ArrayBuffer", Value::make_heap_ptr(AB));
    }

    // ---- %TypedArray%.prototype shared methods ----
    auto proto_m = [&](const char* name, NativeFn fn, uint32_t arity = 0) {
        typed_array_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
    };
    auto as_ta = [](Interpreter& in, Value t) -> JSTypedArray* {
        if (!t.is_heap_ptr() || t.as_heap_ptr()->kind != HeapObject::kTypedArray)
            in.throw_error(u"TypeError", u"method called on a non-TypedArray");
        return static_cast<JSTypedArray*>(t.as_heap_ptr());
    };
    // Allocates a fresh typed array of the same kind as `src` with `len` elements.
    auto new_like = [self, new_buffer](JSTypedArray* src, size_t len) -> JSTypedArray* {
        auto* ta = self->heap_.alloc<JSTypedArray>();
        ta->ta_kind = src->ta_kind;
        ta->proto = src->proto;        // same concrete prototype (e.g. Uint8Array.prototype)
        ta->buffer = new_buffer(len * JSTypedArray::elem_size(src->ta_kind));
        ta->byte_offset = 0;
        ta->length = len;
        return ta;
    };

    proto_m("fill", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value v = arg(a, 0);
        long n = static_cast<long>(ta->length);
        long start = a.size() > 1 ? static_cast<long>(in.to_number(a[1])) : 0;
        long end = (a.size() > 2 && !a[2].is_undefined()) ? static_cast<long>(in.to_number(a[2])) : n;
        if (start < 0) start += n;
        if (end < 0) end += n;
        start = std::max(0L, std::min(start, n));
        end = std::max(0L, std::min(end, n));
        for (long i = start; i < end; ++i) in.ta_set_index(ta, static_cast<size_t>(i), v);
        return t; }, 1);
    proto_m("set", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value src = arg(a, 0);
        size_t off = a.size() > 1 ? static_cast<size_t>(std::max(0.0, in.to_number(a[1]))) : 0;
        if (src.is_heap_ptr() && src.as_heap_ptr()->kind == HeapObject::kTypedArray) {
            auto* s = static_cast<JSTypedArray*>(src.as_heap_ptr());
            if (off + s->length > ta->length) in.throw_error(u"RangeError", u"offset is out of bounds");
            for (size_t i = 0; i < s->length; ++i) in.ta_set_index(ta, off + i, in.ta_get_index(s, i));
        } else {
            double len = in.to_number(in.get_prop_public(src, u"length"));
            size_t n = std::isnan(len) || len < 0 ? 0 : static_cast<size_t>(len);
            if (off + n > ta->length) in.throw_error(u"RangeError", u"offset is out of bounds");
            for (size_t i = 0; i < n; ++i) in.ta_set_index(ta, off + i, in.get_prop_public(src, u16(std::to_string(i))));
        }
        return Value::make_undefined(); }, 2);
    proto_m("subarray", [as_ta, self](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t);
        long n = static_cast<long>(ta->length);
        long begin = a.size() > 0 ? static_cast<long>(in.to_number(a[0])) : 0;
        long end = (a.size() > 1 && !a[1].is_undefined()) ? static_cast<long>(in.to_number(a[1])) : n;
        if (begin < 0) begin += n;
        if (end < 0) end += n;
        begin = std::max(0L, std::min(begin, n));
        end = std::max(0L, std::min(end, n));
        auto* out = self->heap_.alloc<JSTypedArray>();
        out->ta_kind = ta->ta_kind; out->proto = ta->proto; out->buffer = ta->buffer;  // shares the buffer
        out->byte_offset = ta->byte_offset + static_cast<size_t>(begin) * JSTypedArray::elem_size(ta->ta_kind);
        out->length = static_cast<size_t>(std::max(0L, end - begin));
        return Value::make_heap_ptr(out); }, 2);
    proto_m("slice", [as_ta, new_like](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t);
        long n = static_cast<long>(ta->length);
        long begin = a.size() > 0 ? static_cast<long>(in.to_number(a[0])) : 0;
        long end = (a.size() > 1 && !a[1].is_undefined()) ? static_cast<long>(in.to_number(a[1])) : n;
        if (begin < 0) begin += n;
        if (end < 0) end += n;
        begin = std::max(0L, std::min(begin, n));
        end = std::max(0L, std::min(end, n));
        size_t len = static_cast<size_t>(std::max(0L, end - begin));
        JSTypedArray* out = new_like(ta, len);
        for (size_t i = 0; i < len; ++i) in.ta_set_index(out, i, in.ta_get_index(ta, begin + i));
        return Value::make_heap_ptr(out); }, 2);
    proto_m("join", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t);
        std::u16string sep = (a.empty() || a[0].is_undefined()) ? u"," : in.to_string(a[0]);
        std::u16string out;
        for (size_t i = 0; i < ta->length; ++i) { if (i) out += sep; out += in.to_string(in.ta_get_index(ta, i)); }
        return in.str(narrow(out)); }, 1);
    proto_m("indexOf", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value needle = arg(a, 0);
        for (size_t i = 0; i < ta->length; ++i) if (in.strict_equals(in.ta_get_index(ta, i), needle)) return Value::make_int32(static_cast<int32_t>(i));
        return Value::make_int32(-1); }, 1);
    proto_m("includes", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value needle = arg(a, 0);
        for (size_t i = 0; i < ta->length; ++i) if (in.strict_equals(in.ta_get_index(ta, i), needle)) return Value::make_bool(true);
        return Value::make_bool(false); }, 1);
    proto_m("at", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); long n = static_cast<long>(ta->length);
        long i = static_cast<long>(in.to_number(arg(a, 0))); if (i < 0) i += n;
        return (i >= 0 && i < n) ? in.ta_get_index(ta, static_cast<size_t>(i)) : Value::make_undefined(); }, 1);
    proto_m("reverse", [as_ta](Interpreter& in, Value t, std::vector<Value>&) -> Value {
        JSTypedArray* ta = as_ta(in, t);
        for (size_t i = 0, j = ta->length; i + 1 <= j && j-- > i; ++i) {
            Value tmp = in.ta_get_index(ta, i);
            in.ta_set_index(ta, i, in.ta_get_index(ta, j));
            in.ta_set_index(ta, j, tmp);
        }
        return t; }, 0);
    auto iterate = [as_ta](Interpreter& in, Value t, std::vector<Value>& a, char mode) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value cb = arg(a, 0); Value thisArg = arg(a, 1);
        for (size_t i = 0; i < ta->length; ++i) {
            std::vector<Value> ca{in.ta_get_index(ta, i), Value::make_int32(static_cast<int32_t>(i)), t};
            Value r = in.call(cb, thisArg, ca);
            if (mode == 's' && in.to_bool(r)) return Value::make_bool(true);
            if (mode == 'e' && !in.to_bool(r)) return Value::make_bool(false);
            if (mode == 'f' && in.to_bool(r)) return ca[0];
            if (mode == 'i' && in.to_bool(r)) return Value::make_int32(static_cast<int32_t>(i));
        }
        if (mode == 's') return Value::make_bool(false);
        if (mode == 'e') return Value::make_bool(true);
        if (mode == 'i') return Value::make_int32(-1);
        return Value::make_undefined();
    };
    proto_m("forEach", [iterate](Interpreter& in, Value t, std::vector<Value>& a) { return iterate(in, t, a, 'F'); }, 1);
    proto_m("some",    [iterate](Interpreter& in, Value t, std::vector<Value>& a) { return iterate(in, t, a, 's'); }, 1);
    proto_m("every",   [iterate](Interpreter& in, Value t, std::vector<Value>& a) { return iterate(in, t, a, 'e'); }, 1);
    proto_m("find",    [iterate](Interpreter& in, Value t, std::vector<Value>& a) { return iterate(in, t, a, 'f'); }, 1);
    proto_m("findIndex",[iterate](Interpreter& in, Value t, std::vector<Value>& a){ return iterate(in, t, a, 'i'); }, 1);
    proto_m("map", [as_ta, new_like](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value cb = arg(a, 0); Value thisArg = arg(a, 1);
        JSTypedArray* out = new_like(ta, ta->length);
        for (size_t i = 0; i < ta->length; ++i) {
            std::vector<Value> ca{in.ta_get_index(ta, i), Value::make_int32(static_cast<int32_t>(i)), t};
            in.ta_set_index(out, i, in.call(cb, thisArg, ca));
        }
        return Value::make_heap_ptr(out); }, 1);
    proto_m("filter", [as_ta, new_like](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value cb = arg(a, 0); Value thisArg = arg(a, 1);
        std::vector<Value> kept;
        for (size_t i = 0; i < ta->length; ++i) {
            Value e = in.ta_get_index(ta, i);
            std::vector<Value> ca{e, Value::make_int32(static_cast<int32_t>(i)), t};
            if (in.to_bool(in.call(cb, thisArg, ca))) kept.push_back(e);
        }
        JSTypedArray* out = new_like(ta, kept.size());
        for (size_t i = 0; i < kept.size(); ++i) in.ta_set_index(out, i, kept[i]);
        return Value::make_heap_ptr(out); }, 1);
    proto_m("reduce", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value cb = arg(a, 0);
        size_t i = 0; Value acc;
        if (a.size() > 1) acc = a[1];
        else { if (ta->length == 0) in.throw_error(u"TypeError", u"Reduce of empty array with no initial value"); acc = in.ta_get_index(ta, 0); i = 1; }
        for (; i < ta->length; ++i) { std::vector<Value> ca{acc, in.ta_get_index(ta, i), Value::make_int32(static_cast<int32_t>(i)), t}; acc = in.call(cb, Value::make_undefined(), ca); }
        return acc; }, 1);
    proto_m("sort", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); Value cmp = arg(a, 0);
        bool has_cmp = cmp.is_heap_ptr() && cmp.as_heap_ptr()->kind == HeapObject::kJSFunction;
        std::vector<Value> v; v.reserve(ta->length);
        for (size_t i = 0; i < ta->length; ++i) v.push_back(in.ta_get_index(ta, i));
        std::stable_sort(v.begin(), v.end(), [&](Value x, Value y) {
            if (has_cmp) { std::vector<Value> ca{x, y}; return in.to_number(in.call(cmp, Value::make_undefined(), ca)) < 0; }
            return in.to_number(x) < in.to_number(y);
        });
        for (size_t i = 0; i < v.size(); ++i) in.ta_set_index(ta, i, v[i]);
        return t; }, 1);
    auto collect = [as_ta, self](Interpreter& in, Value t, char mode) -> Value {
        JSTypedArray* ta = as_ta(in, t); JSArray* out = self->new_array();
        for (size_t i = 0; i < ta->length; ++i) {
            if (mode == 'k') out->elements.push_back(Value::make_int32(static_cast<int32_t>(i)));
            else if (mode == 'v') out->elements.push_back(in.ta_get_index(ta, i));
            else { JSArray* p = self->new_array(); p->elements = {Value::make_int32(static_cast<int32_t>(i)), in.ta_get_index(ta, i)}; out->elements.push_back(Value::make_heap_ptr(p)); }
        }
        return Value::make_heap_ptr(out);
    };
    proto_m("keys",    [collect](Interpreter& in, Value t, std::vector<Value>&) { return collect(in, t, 'k'); });
    proto_m("values",  [collect](Interpreter& in, Value t, std::vector<Value>&) { return collect(in, t, 'v'); });
    proto_m("entries", [collect](Interpreter& in, Value t, std::vector<Value>&) { return collect(in, t, 'e'); });
    typed_array_proto_->set(u"@@iterator", typed_array_proto_->get(u"values"), false);
    proto_m("toString", [as_ta](Interpreter& in, Value t, std::vector<Value>& a) -> Value {
        JSTypedArray* ta = as_ta(in, t); std::u16string out;
        for (size_t i = 0; i < ta->length; ++i) { if (i) out += u","; out += in.to_string(in.ta_get_index(ta, i)); }
        (void)a; return in.str(narrow(out)); });

    // ---- concrete TypedArray constructors ----
    // Builds a view for `new T(...)`: number length / buffer+offset+length /
    // typed-array copy / array-like or iterable copy.
    for (const TADesc& td : kTypes) {
        TAKind kind = td.kind;
        std::u16string nm = u16(td.name);
        JSObject* proto = heap_.alloc<JSObject>();
        proto->proto = typed_array_proto_;
        size_t es = JSTypedArray::elem_size(kind);

        JSFunction* ctor = new_native(nm, [kind, proto, es, new_buffer, self](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto mk = [&](JSArrayBuffer* buf, size_t off, size_t len) {
                auto* ta = self->heap_.alloc<JSTypedArray>();
                ta->ta_kind = kind; ta->proto = proto; ta->buffer = buf; ta->byte_offset = off; ta->length = len;
                return ta;
            };
            Value first = arg(a, 0);
            if (first.is_heap_ptr() && first.as_heap_ptr()->kind == HeapObject::kArrayBuffer) {
                auto* buf = static_cast<JSArrayBuffer*>(first.as_heap_ptr());
                size_t off = a.size() > 1 ? static_cast<size_t>(std::max(0.0, in.to_number(a[1]))) : 0;
                if (off % es != 0 || off > buf->data.size()) in.throw_error(u"RangeError", u"invalid byteOffset");
                size_t len;
                if (a.size() > 2 && !a[2].is_undefined()) len = static_cast<size_t>(std::max(0.0, in.to_number(a[2])));
                else {
                    if ((buf->data.size() - off) % es != 0) in.throw_error(u"RangeError", u"buffer length not aligned");
                    len = (buf->data.size() - off) / es;
                }
                if (off + len * es > buf->data.size()) in.throw_error(u"RangeError", u"invalid typed array length");
                return Value::make_heap_ptr(mk(buf, off, len));
            }
            if (first.is_heap_ptr() && first.as_heap_ptr()->kind == HeapObject::kTypedArray) {
                auto* s = static_cast<JSTypedArray*>(first.as_heap_ptr());
                JSTypedArray* ta = mk(new_buffer(s->length * es), 0, s->length);
                for (size_t i = 0; i < s->length; ++i) in.ta_set_index(ta, i, in.ta_get_index(s, i));
                return Value::make_heap_ptr(ta);
            }
            if (first.is_heap_ptr()) {  // array-like or iterable
                std::vector<Value> items = in.to_values(first);
                JSTypedArray* ta = mk(new_buffer(items.size() * es), 0, items.size());
                for (size_t i = 0; i < items.size(); ++i) in.ta_set_index(ta, i, items[i]);
                return Value::make_heap_ptr(ta);
            }
            double len = first.is_undefined() ? 0 : in.to_number(first);
            if (std::isnan(len) || len < 0) in.throw_error(u"RangeError", u"Invalid typed array length");
            size_t n = static_cast<size_t>(len);
            return Value::make_heap_ptr(mk(new_buffer(n * es), 0, n));
        }, 3);

        ctor->set(u"prototype", Value::make_heap_ptr(proto), false);
        ctor->set(u"BYTES_PER_ELEMENT", Value::make_int32(static_cast<int32_t>(es)));
        proto->set(u"constructor", Value::make_heap_ptr(ctor), false);
        proto->set(u"BYTES_PER_ELEMENT", Value::make_int32(static_cast<int32_t>(es)), false);
        // T.of(...items) and T.from(arrayLike[, mapFn])
        ctor->set(u"of", Value::make_heap_ptr(new_native(u"of", [ctor](Interpreter& in, Value, std::vector<Value>& a) {
            JSArray* arr = in.new_array(); arr->elements = a;
            std::vector<Value> ca{Value::make_heap_ptr(arr)};
            return in.construct(Value::make_heap_ptr(ctor), ca);
        })));
        ctor->set(u"from", Value::make_heap_ptr(new_native(u"from", [ctor](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::vector<Value> items = in.to_values(arg(a, 0));
            Value mapfn = arg(a, 1);
            bool has_map = mapfn.is_heap_ptr() && mapfn.as_heap_ptr()->kind == HeapObject::kJSFunction;
            if (has_map) for (size_t i = 0; i < items.size(); ++i) { std::vector<Value> ca{items[i], Value::make_int32(static_cast<int32_t>(i))}; items[i] = in.call(mapfn, Value::make_undefined(), ca); }
            JSArray* arr = in.new_array(); arr->elements = items;
            std::vector<Value> ca{Value::make_heap_ptr(arr)};
            return in.construct(Value::make_heap_ptr(ctor), ca);
        }, 1)));
        global_->define(nm, Value::make_heap_ptr(ctor));
    }

    // ---- DataView ----
    {
        JSFunction* DV = new_native(u"DataView", [self](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value b = arg(a, 0);
            if (!b.is_heap_ptr() || b.as_heap_ptr()->kind != HeapObject::kArrayBuffer)
                in.throw_error(u"TypeError", u"First argument to DataView must be an ArrayBuffer");
            auto* buf = static_cast<JSArrayBuffer*>(b.as_heap_ptr());
            size_t off = a.size() > 1 ? static_cast<size_t>(std::max(0.0, in.to_number(a[1]))) : 0;
            if (off > buf->data.size()) in.throw_error(u"RangeError", u"byteOffset out of bounds");
            size_t len = (a.size() > 2 && !a[2].is_undefined())
                             ? static_cast<size_t>(std::max(0.0, in.to_number(a[2]))) : buf->data.size() - off;
            if (off + len > buf->data.size()) in.throw_error(u"RangeError", u"byteLength out of bounds");
            auto* dv = self->heap_.alloc<JSDataView>();
            dv->proto = self->data_view_proto_; dv->buffer = buf; dv->byte_offset = off; dv->byte_length = len;
            return Value::make_heap_ptr(dv);
        }, 1);
        DV->set(u"prototype", Value::make_heap_ptr(data_view_proto_), false);
        data_view_proto_->set(u"constructor", Value::make_heap_ptr(DV), false);

        auto dv_of = [](Interpreter& in, Value t) -> JSDataView* {
            if (!t.is_heap_ptr() || t.as_heap_ptr()->kind != HeapObject::kDataView)
                in.throw_error(u"TypeError", u"method called on a non-DataView");
            return static_cast<JSDataView*>(t.as_heap_ptr());
        };
        auto dv_m = [&](const char* name, NativeFn fn, uint32_t arity) {
            data_view_proto_->set(u16(name), Value::make_heap_ptr(new_native(u16(name), std::move(fn), arity)), false);
        };
        // Reads `bytes` from offset with endianness; bounds-checked.
        auto read = [dv_of](Interpreter& in, Value t, std::vector<Value>& a, size_t bytes, bool little_default) -> std::array<uint8_t, 8> {
            JSDataView* dv = dv_of(in, t);
            long off = static_cast<long>(in.to_number(arg(a, 0)));
            bool le = bytes == 1 ? true : (a.size() > 1 ? in.to_bool(a[1]) : little_default);
            if (off < 0 || static_cast<size_t>(off) + bytes > dv->byte_length) in.throw_error(u"RangeError", u"Offset is outside the bounds of the DataView");
            std::array<uint8_t, 8> buf{};
            const uint8_t* p = dv->buffer->data.data() + dv->byte_offset + off;
            for (size_t i = 0; i < bytes; ++i) buf[i] = le ? p[i] : p[bytes - 1 - i];
            return buf;
        };
        auto write = [dv_of](Interpreter& in, Value t, std::vector<Value>& a, size_t bytes, const uint8_t* src) {
            JSDataView* dv = dv_of(in, t);
            long off = static_cast<long>(in.to_number(arg(a, 0)));
            bool le = bytes == 1 ? true : (a.size() > 2 ? in.to_bool(a[2]) : false);
            if (off < 0 || static_cast<size_t>(off) + bytes > dv->byte_length) in.throw_error(u"RangeError", u"Offset is outside the bounds of the DataView");
            uint8_t* p = dv->buffer->data.data() + dv->byte_offset + off;
            for (size_t i = 0; i < bytes; ++i) p[i] = le ? src[i] : src[bytes - 1 - i];
        };
        dv_m("getInt8",  [read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,1,true); return Value::make_int32(static_cast<int8_t>(b[0])); }, 1);
        dv_m("getUint8", [read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,1,true); return Value::make_int32(b[0]); }, 1);
        dv_m("getInt16", [read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,2,false); int16_t v; std::memcpy(&v,b.data(),2); return Value::make_int32(v); }, 1);
        dv_m("getUint16",[read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,2,false); uint16_t v; std::memcpy(&v,b.data(),2); return Value::make_int32(v); }, 1);
        dv_m("getInt32", [read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,4,false); int32_t v; std::memcpy(&v,b.data(),4); return Value::make_int32(v); }, 1);
        dv_m("getUint32",[read](Interpreter& in, Value t, std::vector<Value>& a) { auto b = read(in,t,a,4,false); uint32_t v; std::memcpy(&v,b.data(),4); return Value::make_double(static_cast<double>(v)); }, 1);
        dv_m("getFloat32",[read](Interpreter& in, Value t, std::vector<Value>& a){ auto b = read(in,t,a,4,false); float v; std::memcpy(&v,b.data(),4); return Value::make_double(static_cast<double>(v)); }, 1);
        dv_m("getFloat64",[read](Interpreter& in, Value t, std::vector<Value>& a){ auto b = read(in,t,a,8,false); double v; std::memcpy(&v,b.data(),8); return Value::make_double(v); }, 1);
        dv_m("setInt8",  [write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { int8_t v = static_cast<int8_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,1,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setUint8", [write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { uint8_t v = static_cast<uint8_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,1,&v); return Value::make_undefined(); }, 2);
        dv_m("setInt16", [write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { int16_t v = static_cast<int16_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,2,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setUint16",[write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { uint16_t v = static_cast<uint16_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,2,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setInt32", [write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { int32_t v = static_cast<int32_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,4,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setUint32",[write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(in.to_number(arg(a,1)))); write(in,t,a,4,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setFloat32",[write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { float v = static_cast<float>(in.to_number(arg(a,1))); write(in,t,a,4,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        dv_m("setFloat64",[write](Interpreter& in, Value t, std::vector<Value>& a) -> Value { double v = in.to_number(arg(a,1)); write(in,t,a,8,reinterpret_cast<uint8_t*>(&v)); return Value::make_undefined(); }, 2);
        global_->define(u"DataView", Value::make_heap_ptr(DV));
    }
}

} // namespace malibu::js::runtime
