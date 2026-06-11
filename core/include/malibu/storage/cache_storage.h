#pragma once
// core/include/malibu/storage/cache_storage.h
// Cache API (Service Worker specification) — named caches of request→response.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace malibu::storage {

class CacheStorage {
public:
    struct CachedResponse {
        int32_t                            status = 200;
        std::map<std::string, std::string> headers;
        std::vector<uint8_t>               body;
    };

    void open(const std::string& cache_name) { caches_[cache_name]; }
    [[nodiscard]] bool has(const std::string& cache_name) const { return caches_.count(cache_name) != 0; }
    bool delete_cache(const std::string& cache_name) { return caches_.erase(cache_name) != 0; }
    [[nodiscard]] std::vector<std::string> keys() const;

    void put(const std::string& cache_name, const std::string& request_url, CachedResponse resp);
    [[nodiscard]] std::optional<CachedResponse> match(const std::string& cache_name,
                                                      const std::string& request_url) const;
    bool delete_entry(const std::string& cache_name, const std::string& request_url);

    [[nodiscard]] bool empty() const noexcept { return caches_.empty(); }

private:
    std::map<std::string, std::map<std::string, CachedResponse>> caches_;
};

} // namespace malibu::storage
