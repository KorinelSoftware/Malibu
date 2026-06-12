// storage/cookie_store.cpp
// Cookie jar with RFC 6265 attribute enforcement.

#include "malibu/storage/cookie_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

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

std::string trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string default_cookie_path(const std::string& request_path) {
    if (request_path.empty() || request_path.front() != '/') return "/";
    const size_t slash = request_path.rfind('/');
    if (slash == 0 || slash == std::string::npos) return "/";
    return request_path.substr(0, slash);
}

uint64_t unix_time_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
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

bool CookieStore::set_cookie_from_document(const std::string& url,
                                           const std::string& assignment) {
    const UrlParts source = parse_url(url);
    if (source.host.empty()) return false;

    const size_t semicolon = assignment.find(';');
    const std::string pair =
        trim(assignment.substr(0, semicolon));
    const size_t equals = pair.find('=');
    if (equals == std::string::npos) return false;

    Cookie cookie;
    cookie.name = trim(pair.substr(0, equals));
    cookie.value = trim(pair.substr(equals + 1));
    cookie.domain = source.host;
    cookie.path = default_cookie_path(source.path);
    if (cookie.name.empty() ||
        cookie.name.find_first_of("()<>@,;:\\\"/[]?={} \t\r\n") !=
            std::string::npos) {
        return false;
    }

    bool delete_now = false;
    size_t cursor =
        semicolon == std::string::npos ? assignment.size() : semicolon + 1;
    while (cursor < assignment.size()) {
        const size_t next = assignment.find(';', cursor);
        std::string attribute = trim(
            assignment.substr(cursor, next == std::string::npos
                                          ? std::string::npos
                                          : next - cursor));
        const size_t attribute_equals = attribute.find('=');
        const std::string key = lower_ascii(trim(attribute.substr(
            0, attribute_equals)));
        const std::string value =
            attribute_equals == std::string::npos
                ? std::string()
                : trim(attribute.substr(attribute_equals + 1));

        if (key == "domain") {
            std::string domain = lower_ascii(value);
            while (!domain.empty() && domain.front() == '.')
                domain.erase(domain.begin());
            if (domain.empty() || !domain_matches(source.host, domain))
                return false;
            cookie.domain = std::move(domain);
        } else if (key == "path") {
            cookie.path =
                !value.empty() && value.front() == '/' ? value : "/";
        } else if (key == "secure") {
            if (source.scheme != "https") return false;
            cookie.secure = true;
        } else if (key == "samesite") {
            const std::string mode = lower_ascii(value);
            if (mode == "none")
                cookie.same_site = Cookie::SameSite::None;
            else if (mode == "strict")
                cookie.same_site = Cookie::SameSite::Strict;
            else
                cookie.same_site = Cookie::SameSite::Lax;
        } else if (key == "max-age") {
            char* end = nullptr;
            const long long seconds = std::strtoll(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0') {
                if (seconds <= 0) {
                    delete_now = true;
                } else {
                    cookie.expires =
                        unix_time_seconds() +
                        static_cast<uint64_t>(seconds);
                }
            }
        }

        if (next == std::string::npos) break;
        cursor = next + 1;
    }

    if (delete_now) {
        cookies_.erase(
            std::remove_if(cookies_.begin(), cookies_.end(),
                           [&](const Cookie& existing) {
                               return existing.name == cookie.name &&
                                      existing.domain == cookie.domain &&
                                      existing.path == cookie.path;
                           }),
            cookies_.end());
        return true;
    }
    set_cookie(url, std::move(cookie));
    return true;
}

std::vector<Cookie> CookieStore::get_cookies(const std::string& url, const Context& ctx) const {
    UrlParts u = parse_url(url);
    std::vector<Cookie> out;
    const uint64_t now = unix_time_seconds();
    for (const Cookie& c : cookies_) {
        if (c.expires != 0 && c.expires <= now) continue;
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
