#pragma once
// core/include/malibu/js/vm/realm.h
// JS Realm isolation.

#include <cstdint>
#include <string_view>
#include "value.h"
#include "../../types.h"

namespace malibu::js::vm {

using RealmId = uint32_t;

class JSRealm {
public:
    JSRealm();
    RealmId id() const noexcept;
    Value get_global(std::string_view name);
    void set_global(std::string_view name, Value val);
    JSObjectHandle create_dom_wrapper(malibu::NodeHandle h);  // Creates DOM wrapper for node
    // ... more realm methods
private:
    RealmId id_;
    static RealmId s_next_id;
};

} // namespace malibu::js::vm