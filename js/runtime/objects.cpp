// js/runtime/objects.cpp
// Property/environment operations for the runtime object model.

#include "malibu/js/runtime/objects.h"

namespace malibu::js::runtime {

// ---- JSObject -------------------------------------------------------------
Property* JSObject::find_own(std::u16string_view key) {
    for (auto& p : props) if (p.key == key) return &p;
    return nullptr;
}
const Property* JSObject::find_own(std::u16string_view key) const {
    for (auto& p : props) if (p.key == key) return &p;
    return nullptr;
}

Property* JSObject::resolve(std::u16string_view key) {
    for (JSObject* o = this; o; o = o->proto)
        if (Property* p = o->find_own(key)) return p;
    return nullptr;
}

void JSObject::define_accessor(std::u16string_view key, Value fn, bool is_setter) {
    Property* p = find_own(key);
    if (!p) { props.push_back(Property{std::u16string(key), Value::make_undefined(), false, false, {}, {}}); p = &props.back(); }
    p->is_accessor = true;
    if (is_setter) p->setter = fn; else p->getter = fn;
}

Value JSObject::get(std::u16string_view key) const {
    const JSObject* o = this;
    while (o) {
        if (const Property* p = o->find_own(key)) return p->value;
        o = o->proto;
    }
    return Value::make_undefined();
}

bool JSObject::has_own(std::u16string_view key) const { return find_own(key) != nullptr; }

bool JSObject::has(std::u16string_view key) const {
    const JSObject* o = this;
    while (o) { if (o->find_own(key)) return true; o = o->proto; }
    return false;
}

void JSObject::set(std::u16string_view key, Value v, bool enumerable) {
    if (Property* p = find_own(key)) {
        p->value = v;
        p->is_accessor = false;  // overwriting an accessor with a data value
        return;
    }
    props.push_back(Property{std::u16string(key), v, enumerable, false, {}, {}});
}

bool JSObject::delete_prop(std::u16string_view key) {
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (it->key == key) { props.erase(it); return true; }
    }
    return false;
}

std::vector<std::u16string> JSObject::own_enumerable_keys() const {
    std::vector<std::u16string> out;
    // Private names (`#x`) and engine-internal keys (`%...%`) are never observable
    // via enumeration (Object.keys / for-in / JSON / spread).
    for (const auto& p : props)
        if (p.enumerable && !p.key.empty() && p.key[0] != u'#' && p.key[0] != u'%')
            out.push_back(p.key);
    return out;
}

// ---- JSFunction -----------------------------------------------------------
Property* JSFunction::find_own(std::u16string_view key) {
    for (auto& p : props) if (p.key == key) return &p;
    return nullptr;
}
Value JSFunction::get(std::u16string_view key) const {
    for (auto& p : props) if (p.key == key) return p.value;
    return Value::make_undefined();
}
void JSFunction::set(std::u16string_view key, Value v) {
    for (auto& p : props) if (p.key == key) { p.value = v; return; }
    props.push_back(Property{std::u16string(key), v, true, false, {}, {}});
}

// ---- Environment ----------------------------------------------------------
Value* Environment::find(std::u16string_view name) {
    for (Environment* e = this; e; e = e->parent) {
        if (e->object_backing) {
            Property* p = e->is_with ? e->object_backing->resolve(name)
                                     : e->object_backing->find_own(name);
            if (p) return &p->value;
        } else {
            for (auto& [k, v] : e->slots) if (k == name) return &v;
        }
    }
    return nullptr;
}
void Environment::define(std::u16string_view name, Value v) {
    if (object_backing) { object_backing->set(name, v); return; }
    for (auto& [k, val] : slots) if (k == name) { val = v; return; }
    slots.emplace_back(std::u16string(name), v);
}
bool Environment::set(std::u16string_view name, Value v) {
    if (Value* slot = find(name)) { *slot = v; return true; }
    return false;
}
bool Environment::has(std::u16string_view name) const {
    for (const Environment* e = this; e; e = e->parent) {
        for (auto& [k, v] : e->slots) if (k == name) return true;
    }
    return false;
}

} // namespace malibu::js::runtime
