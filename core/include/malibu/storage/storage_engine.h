#pragma once
// core/include/malibu/storage/storage_engine.h
// Per-origin storage isolation (Task 24 / Requirement 12). Owns localStorage,
// sessionStorage, IndexedDB, CacheStorage, and the shared cookie jar, keyed by
// Origin so one origin can never read another's data.

#include <string>
#include <unordered_map>

#include "malibu/security/origin.h"
#include "malibu/storage/local_storage.h"
#include "malibu/storage/indexed_db.h"
#include "malibu/storage/cache_storage.h"
#include "malibu/storage/cookie_store.h"

namespace malibu::storage {

struct OriginHash {
    size_t operator()(const malibu::security::Origin& o) const noexcept {
        std::hash<std::string> hs;
        size_t h = hs(o.scheme);
        h ^= hs(o.host) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(o.port) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

class StorageEngine {
public:
    using Origin = malibu::security::Origin;

    // Per-origin accessors. Each returns the storage for THAT origin only.
    Storage&      local_storage(const Origin& origin) { return ls_[origin]; }
    Storage&      session_storage(const Origin& origin, const std::string& session_id) {
        return ss_[OriginSession{origin, session_id}];
    }
    IndexedDB&    indexed_db(const Origin& origin) { return idb_[origin]; }
    CacheStorage& cache_storage(const Origin& origin) { return cs_[origin]; }
    CookieStore&  cookies() noexcept { return cookies_; }

    // Atomically removes every storage type for `origin` (Req 12.6). In-memory
    // backing makes this naturally all-or-nothing.
    bool clear_origin(const Origin& origin);

    [[nodiscard]] bool has_local_storage(const Origin& origin) const { return ls_.count(origin) != 0; }
    [[nodiscard]] bool has_indexed_db(const Origin& origin) const { return idb_.count(origin) != 0; }

private:
    struct OriginSession {
        Origin      origin;
        std::string session_id;
        bool operator==(const OriginSession& o) const noexcept {
            return origin == o.origin && session_id == o.session_id;
        }
    };
    struct OriginSessionHash {
        size_t operator()(const OriginSession& s) const noexcept {
            return OriginHash{}(s.origin) ^ (std::hash<std::string>{}(s.session_id) << 1);
        }
    };

    std::unordered_map<Origin, Storage, OriginHash>                       ls_;
    std::unordered_map<OriginSession, Storage, OriginSessionHash>          ss_;
    std::unordered_map<Origin, IndexedDB, OriginHash>                      idb_;
    std::unordered_map<Origin, CacheStorage, OriginHash>                   cs_;
    CookieStore                                                            cookies_;
};

} // namespace malibu::storage
