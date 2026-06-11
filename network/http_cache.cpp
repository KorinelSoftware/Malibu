// network/http_cache.cpp
// RFC 7234 response caching: parse Cache-Control / ETag / Last-Modified.

#include "malibu/network/http_cache.h"

#include <algorithm>
#include <cctype>

namespace malibu::network {
namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
}  // namespace

void HttpCache::store(const std::string& url, const FetchResponse& resp) {
    CacheEntry e;
    e.response = resp;
    e.etag = resp.headers.get("ETag");
    e.last_modified = resp.headers.get("Last-Modified");

    std::string cc = lower(resp.headers.get("Cache-Control"));
    size_t pos = 0;
    while (pos < cc.size()) {
        size_t comma = cc.find(',', pos);
        std::string token = trim(cc.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (token == "no-store") e.no_store = true;
        else if (token == "no-cache") e.no_cache = true;
        else if (token.rfind("max-age=", 0) == 0) {
            e.has_max_age = true;
            try { e.max_age = std::stol(token.substr(8)); } catch (...) { e.max_age = 0; }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    if (e.no_store) { entries_.erase(url); return; }
    entries_[url] = std::move(e);
}

std::optional<CacheEntry> HttpCache::lookup(const std::string& url) const {
    auto it = entries_.find(url);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

} // namespace malibu::network
