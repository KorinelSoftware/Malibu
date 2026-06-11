#pragma once
// core/include/malibu/network/fetch_types.h
// Shared WHATWG Fetch data types (Request / Response / Headers / enums).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace malibu::network {

// Case-insensitive header map (HTTP header names are case-insensitive).
struct HeaderMap {
    std::map<std::string, std::string> entries;  // keys stored lowercased

    static std::string lower(std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
    void set(const std::string& k, const std::string& v) { entries[lower(k)] = v; }
    [[nodiscard]] bool has(const std::string& k) const { return entries.count(lower(k)) != 0; }
    [[nodiscard]] std::string get(const std::string& k) const {
        auto it = entries.find(lower(k));
        return it != entries.end() ? it->second : std::string();
    }
};

enum class CredentialsMode : uint8_t { Omit, SameOrigin, Include };
enum class CacheMode : uint8_t { Default, NoStore, Reload, NoCache, ForceCache, OnlyIfCached };
enum class RedirectMode : uint8_t { Follow, Error, Manual };
enum class ResponseType : uint8_t { Basic, Cors, Opaque, Error };

struct FetchRequest {
    std::string          url;
    std::string          method = "GET";
    HeaderMap            headers;
    std::vector<uint8_t> body;
    CredentialsMode      credentials = CredentialsMode::SameOrigin;
    CacheMode            cache = CacheMode::Default;
    RedirectMode         redirect = RedirectMode::Follow;
    std::string          referrer;
    std::string          integrity;
};

struct FetchResponse {
    int32_t              status = 0;
    std::string          status_text;
    HeaderMap            headers;
    std::vector<uint8_t> body;
    bool                 ok = false;
    std::string          url;
    ResponseType         type = ResponseType::Error;

    [[nodiscard]] bool is_network_error() const noexcept { return type == ResponseType::Error; }
};

} // namespace malibu::network
