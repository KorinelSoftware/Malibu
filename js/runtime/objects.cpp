// js/runtime/objects.cpp
// Property/environment operations for the runtime object model.

#include "malibu/js/runtime/objects.h"

#include <algorithm>

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

void JSObject::define_accessor(std::u16string_view key, Value fn, bool is_setter,
                               bool enumerable) {
    Property* p = find_own(key);
    if (!p) {
        props.push_back(Property{std::u16string(key), Value::make_undefined(),
                                 enumerable, false, {}, {}});
        p = &props.back();
    }
    p->enumerable = enumerable;
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
        if (!p->writable || p->is_accessor) return;
        p->value = v;
        return;
    }
    if (!extensible) return;
    props.push_back(Property{std::u16string(key), v, enumerable, false, {}, {}});
}

bool JSObject::delete_prop(std::u16string_view key) {
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (it->key == key) {
            if (!it->configurable) return false;
            props.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<std::u16string> JSObject::own_enumerable_keys() const {
    std::vector<std::u16string> out;
    // Private names (`#x`) and engine-internal keys (`%...%`) are never observable
    // via enumeration (Object.keys / for-in / JSON / spread).
    for (const auto& p : props)
        if (p.enumerable && !p.key.empty() && p.key[0] != u'#' &&
            p.key[0] != u'%' && p.key.rfind(u"@@", 0) != 0)
            out.push_back(p.key);
    return out;
}

// ---- JSArray --------------------------------------------------------------
bool JSArray::has_index(size_t index) const noexcept {
    if (index >= elements.size()) return false;
    return presence.empty() || index >= presence.size() || presence[index] != 0;
}

void JSArray::materialize_presence() {
    if (presence.empty()) presence.assign(elements.size(), 1);
    else if (presence.size() < elements.size()) presence.resize(elements.size(), 1);
}

void JSArray::normalize_presence() {
    if (presence.empty()) return;
    if (presence.size() < elements.size()) presence.resize(elements.size(), 1);
    if (presence.size() > elements.size()) presence.resize(elements.size());
    bool dense = true;
    for (uint8_t present : presence) {
        if (!present) {
            dense = false;
            break;
        }
    }
    if (dense) presence.clear();
}

void JSArray::append(Value value, bool present) {
    if (!present && presence.empty()) presence.assign(elements.size(), 1);
    else if (!presence.empty()) materialize_presence();
    elements.push_back(value);
    if (!present || !presence.empty()) presence.push_back(present ? 1 : 0);
}

void JSArray::resize_length(size_t length, bool new_indices_present) {
    size_t old_length = elements.size();
    if (length > old_length && !new_indices_present && presence.empty())
        presence.assign(old_length, 1);
    else if (!presence.empty()) materialize_presence();
    elements.resize(length, Value::make_undefined());
    if (!presence.empty() || (length > old_length && !new_indices_present))
        presence.resize(length, new_indices_present ? 1 : 0);
    normalize_presence();
}

void JSArray::set_index(size_t index, Value value) {
    if (index >= elements.size()) {
        resize_length(index + 1, false);
    }
    elements[index] = value;
    if (!presence.empty()) {
        materialize_presence();
        presence[index] = 1;
        normalize_presence();
    }
}

void JSArray::delete_index(size_t index) {
    if (index >= elements.size() || !has_index(index)) return;
    materialize_presence();
    presence[index] = 0;
    elements[index] = Value::make_undefined();
}

void JSArray::erase_range(size_t start, size_t count) {
    if (start >= elements.size() || count == 0) return;
    count = std::min(count, elements.size() - start);
    if (!presence.empty()) materialize_presence();
    elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(start),
                   elements.begin() + static_cast<std::ptrdiff_t>(start + count));
    if (!presence.empty()) {
        presence.erase(presence.begin() + static_cast<std::ptrdiff_t>(start),
                       presence.begin() + static_cast<std::ptrdiff_t>(start + count));
        normalize_presence();
    }
}

void JSArray::insert_dense(size_t start, const std::vector<Value>& values) {
    start = std::min(start, elements.size());
    if (!presence.empty()) materialize_presence();
    elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(start),
                    values.begin(), values.end());
    if (!presence.empty()) {
        presence.insert(presence.begin() + static_cast<std::ptrdiff_t>(start),
                        values.size(), 1);
        normalize_presence();
    }
}

void JSArray::reverse_elements() {
    std::reverse(elements.begin(), elements.end());
    if (!presence.empty()) {
        materialize_presence();
        std::reverse(presence.begin(), presence.end());
        normalize_presence();
    }
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
void Environment::define_if_absent(std::u16string_view name, Value v) {
    if (object_backing) {
        if (!object_backing->find_own(name))
            object_backing->set(name, v);
        return;
    }
    for (auto& [key, value] : slots) {
        (void)value;
        if (key == name)
            return;
    }
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
