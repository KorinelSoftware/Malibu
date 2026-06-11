// storage/cache_storage.cpp
// Cache API named-cache store.

#include "malibu/storage/cache_storage.h"

namespace malibu::storage {

std::vector<std::string> CacheStorage::keys() const {
    std::vector<std::string> names;
    names.reserve(caches_.size());
    for (const auto& [name, entries] : caches_) names.push_back(name);
    return names;
}

void CacheStorage::put(const std::string& cache_name, const std::string& request_url, CachedResponse resp) {
    caches_[cache_name][request_url] = std::move(resp);
}

std::optional<CacheStorage::CachedResponse> CacheStorage::match(const std::string& cache_name,
                                                                const std::string& request_url) const {
    auto cit = caches_.find(cache_name);
    if (cit == caches_.end()) return std::nullopt;
    auto eit = cit->second.find(request_url);
    if (eit == cit->second.end()) return std::nullopt;
    return eit->second;
}

bool CacheStorage::delete_entry(const std::string& cache_name, const std::string& request_url) {
    auto cit = caches_.find(cache_name);
    if (cit == caches_.end()) return false;
    return cit->second.erase(request_url) != 0;
}

} // namespace malibu::storage
