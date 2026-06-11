#pragma once
// core/include/malibu/dom/wrapper_cache.h
// Wrapper Cache for DOM node -> JS wrapper mapping.

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include "../types.h"
#include "malibu/js/vm/realm.h"

namespace malibu::dom {

using RealmId = uint32_t;

struct WrapperKey {
    uint32_t node_index;
    uint32_t node_generation;
    RealmId realm_id;
    
    bool operator==(const WrapperKey&) const noexcept = default;
};

struct WrapperKeyHash {
    size_t operator()(const WrapperKey& k) const noexcept;
};

class WrapperCache {
public:
    // Returns existing wrapper for (handle, realm), or creates one.
    // Thread-safe: multiple realms may call concurrently.
    malibu::JSObjectHandle get_or_create(malibu::NodeHandle h, malibu::js::vm::JSRealm& realm);
    
    // Called by GC when a wrapper is collected.
    void on_wrapper_collected(malibu::NodeHandle h, RealmId realm_id);
    
    // Called when a realm is destroyed.
    void purge_realm(RealmId realm_id);
private:
    std::unordered_map<WrapperKey, malibu::JSObjectHandle, WrapperKeyHash> map_;
    std::mutex mu_;
};

} // namespace malibu::dom