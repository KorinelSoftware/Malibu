// js/vm/realm.cpp
// JSRealm — per-document JS isolation unit.

#include "malibu/js/vm/realm.h"

#include <unordered_map>
#include <string>

namespace malibu::js::vm {

RealmId JSRealm::s_next_id = 1;

JSRealm::JSRealm() : id_(s_next_id++) {}

RealmId JSRealm::id() const noexcept { return id_; }

Value JSRealm::get_global(std::string_view) { return Value::make_undefined(); }
void  JSRealm::set_global(std::string_view, Value) {}

JSObjectHandle JSRealm::create_dom_wrapper(malibu::NodeHandle h) {
    // A stable, per-(realm, node) wrapper identity for the WrapperCache. The
    // real implementation allocates a DomNodeRef on the JS heap; this encoding
    // keeps the identity unique per realm + node until the GC lands.
    uint64_t combined = (static_cast<uint64_t>(id_) << 40)
                        ^ (static_cast<uint64_t>(h.index) << 8)
                        ^ static_cast<uint64_t>(h.generation);
    return JSObjectHandle{combined ? combined : 1};
}

} // namespace malibu::js::vm
