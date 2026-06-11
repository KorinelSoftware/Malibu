#pragma once
// core/include/malibu/storage/cookie_store.h
// HTTP cookies per RFC 6265, including the HttpOnly / Secure / SameSite rules.

#include <cstdint>
#include <string>
#include <vector>

namespace malibu::storage {

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;   // host the cookie applies to (no leading dot)
    std::string path = "/";
    uint64_t    expires = 0;       // 0 = session cookie
    bool        secure = false;
    bool        http_only = false;
    enum class SameSite : uint8_t { None, Lax, Strict } same_site = SameSite::Lax;
};

class CookieStore {
public:
    // Context of a retrieval, used to enforce Secure / SameSite / HttpOnly.
    struct Context {
        bool secure_transport = true;   // request is over HTTPS
        bool same_site = true;          // request is same-site as the cookie's site
        bool script_access = false;     // true for document.cookie (excludes HttpOnly)
    };

    // Stores a cookie for `url` (domain/path default to the URL's host/path).
    void set_cookie(const std::string& url, Cookie cookie);

    // Cookies applicable to `url` under `ctx` (filters Secure/SameSite/HttpOnly).
    [[nodiscard]] std::vector<Cookie> get_cookies(const std::string& url, const Context& ctx) const;

    // The `document.cookie` string for a same-site, script context.
    [[nodiscard]] std::string get_cookie_string(const std::string& url) const;

    void delete_cookie(const std::string& url, const std::string& name);
    void clear_host(const std::string& host);
    void clear() { cookies_.clear(); }
    [[nodiscard]] size_t size() const noexcept { return cookies_.size(); }

private:
    std::vector<Cookie> cookies_;
};

} // namespace malibu::storage
