// storage/cookie_store.cpp
// Cookie jar with RFC 6265 attribute enforcement.

#include "malibu/storage/cookie_store.h"

#include <algorithm>

namespace malibu::storage {
namespace {

struct UrlParts { std::string scheme, host, path = "/"; };

UrlParts parse_url(const std::string& url) {
    UrlParts u;
    auto sep = url.find("://");
    size_t host_start = 0;
    if (sep != std::string::npos) { u.scheme = url.substr(0, sep); host_start = sep + 3; }
    size_t path_start = url.find('/', host_start);
    std::string authority = (path_start == std::string::npos) ? url.substr(host_start)
                                                              : url.substr(host_start, path_start - host_start);
    if (auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    if (auto colon = authority.rfind(':'); colon != std::string::npos) authority = authority.substr(0, colon);
    std::transform(authority.begin(), authority.end(), authority.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    u.host = authority;
    if (path_start != std::string::npos) {
        size_t end = url.find_first_of("?#", path_start);
        u.path = url.substr(path_start, end == std::string::npos ? std::string::npos : end - path_start);
    }
    if (u.path.empty()) u.path = "/";
    return u;
}

bool domain_matches(const std::string& host, const std::string& domain) {
    if (host == domain) return true;
    return host.size() > domain.size() &&
           host.compare(host.size() - domain.size() - 1, domain.size() + 1, "." + domain) == 0;
}

bool path_matches(const std::string& request_path, const std::string& cookie_path) {
    if (request_path == cookie_path) return true;
    if (request_path.rfind(cookie_path, 0) != 0) return false;
    return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
}

}  // namespace

void CookieStore::set_cookie(const std::string& url, Cookie cookie) {
    UrlParts u = parse_url(url);
    if (cookie.domain.empty()) cookie.domain = u.host;
    if (cookie.path.empty()) cookie.path = "/";
    // Replace an existing cookie with the same (domain, path, name).
    for (auto& c : cookies_) {
        if (c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path) {
            c = std::move(cookie);
            return;
        }
    }
    cookies_.push_back(std::move(cookie));
}

std::vector<Cookie> CookieStore::get_cookies(const std::string& url, const Context& ctx) const {
    UrlParts u = parse_url(url);
    std::vector<Cookie> out;
    for (const Cookie& c : cookies_) {
        if (!domain_matches(u.host, c.domain)) continue;
        if (!path_matches(u.path, c.path)) continue;
        if (c.secure && !ctx.secure_transport) continue;          // Secure: HTTPS only
        if (c.http_only && ctx.script_access) continue;            // HttpOnly: not for scripts
        if (!ctx.same_site && c.same_site != Cookie::SameSite::None) continue;  // SameSite: cross-site filter
        out.push_back(c);
    }
    return out;
}

std::string CookieStore::get_cookie_string(const std::string& url) const {
    UrlParts u = parse_url(url);
    Context ctx;
    ctx.secure_transport = (u.scheme == "https" || u.scheme == "wss");
    ctx.same_site = true;
    ctx.script_access = true;  // document.cookie excludes HttpOnly
    std::string out;
    for (const Cookie& c : get_cookies(url, ctx)) {
        if (!out.empty()) out += "; ";
        out += c.name + "=" + c.value;
    }
    return out;
}

void CookieStore::delete_cookie(const std::string& url, const std::string& name) {
    UrlParts u = parse_url(url);
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                  [&](const Cookie& c) { return c.name == name && domain_matches(u.host, c.domain); }),
                   cookies_.end());
}

void CookieStore::clear_host(const std::string& host) {
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                  [&](const Cookie& c) { return domain_matches(host, c.domain); }),
                   cookies_.end());
}

} // namespace malibu::storage
