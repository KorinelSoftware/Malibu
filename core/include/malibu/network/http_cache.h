#pragma once
// core/include/malibu/network/http_cache.h
// HTTP cache per RFC 7234: stores responses, parses Cache-Control / ETag /
// Last-Modified, and supports conditional revalidation.

#include <optional>
#include <string>
#include <unordered_map>

#include "malibu/network/fetch_types.h"

namespace malibu::network {

struct CacheEntry {
    FetchResponse response;
    std::string   etag;
    std::string   last_modified;
    bool          no_store = false;
    bool          no_cache = false;     // must revalidate before use
    bool          has_max_age = false;
    long          max_age = 0;
    [[nodiscard]] bool has_validator() const { return !etag.empty() || !last_modified.empty(); }
};

class HttpCache {
public:
    // Parses caching headers and stores `resp` for `url` (unless no-store).
    void store(const std::string& url, const FetchResponse& resp);

    [[nodiscard]] std::optional<CacheEntry> lookup(const std::string& url) const;
    void remove(const std::string& url) { entries_.erase(url); }
    void clear() { entries_.clear(); }
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

private:
    std::unordered_map<std::string, CacheEntry> entries_;
};

} // namespace malibu::network
