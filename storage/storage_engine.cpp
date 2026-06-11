// storage/storage_engine.cpp
// Per-origin storage isolation and atomic origin clearing.

#include "malibu/storage/storage_engine.h"

namespace malibu::storage {

bool StorageEngine::clear_origin(const Origin& origin) {
    // Remove every storage type for this origin. In-memory erases are atomic
    // (no partially-cleared state is ever observable).
    ls_.erase(origin);
    idb_.erase(origin);
    cs_.erase(origin);
    for (auto it = ss_.begin(); it != ss_.end();) {
        if (it->first.origin == origin) it = ss_.erase(it);
        else ++it;
    }
    cookies_.clear_host(origin.host);
    return true;
}

} // namespace malibu::storage
