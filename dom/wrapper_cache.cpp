// dom/wrapper_cache.cpp
// Wrapper Cache implementation.

#include "malibu/dom/wrapper_cache.h"
#include "malibu/js/vm/realm.h"

namespace malibu::dom {

size_t WrapperKeyHash::operator()(const WrapperKey& k) const noexcept {
    // FNV-1a 64-bit hash over three fields
    size_t h = 2166136261u;
    h ^= k.node_index;      h *= 16777619u;
    h ^= k.node_generation; h *= 16777619u;
    h ^= k.realm_id;        h *= 16777619u;
    return h;
}

malibu::JSObjectHandle WrapperCache::get_or_create(malibu::NodeHandle h, malibu::js::vm::JSRealm& realm) {
    WrapperKey key{h.index, h.generation, realm.id()};
    
    // Fast path: check without lock (double-checked locking)
    {
        std::lock_guard lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            return it->second;
        }
    }
    
    // Slow path: create wrapper (lock already held from above)
    std::lock_guard lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        return it->second;
    }
    
    // Create new wrapper for this realm
    malibu::JSObjectHandle wrapper = realm.create_dom_wrapper(h);
    map_.emplace(key, wrapper);
    return wrapper;
}

void WrapperCache::on_wrapper_collected(malibu::NodeHandle h, RealmId realm_id) {
    std::lock_guard lock(mu_);
    WrapperKey key{h.index, h.generation, realm_id};
    map_.erase(key);
}

void WrapperCache::purge_realm(RealmId realm_id) {
    std::lock_guard lock(mu_);
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->first.realm_id == realm_id) {
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace malibu::dom