// security/origin.cpp
// Origin parsing, serialization, and same-origin comparison.

#include "malibu/security/origin.h"

#include <algorithm>
#include <atomic>
#include <cctype>

namespace malibu::security {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint16_t default_port(const std::string& scheme) {
    if (scheme == "http"  || scheme == "ws")  return 80;
    if (scheme == "https" || scheme == "wss") return 443;
    if (scheme == "ftp")  return 21;
    return 0;
}

bool scheme_has_authority(const std::string& scheme) {
    return scheme == "http" || scheme == "https" || scheme == "ws" ||
           scheme == "wss" || scheme == "ftp" || scheme == "file";
}

}  // namespace

bool Origin::is_opaque() const noexcept { return scheme == "null"; }

bool Origin::same_origin(const Origin& other) const noexcept {
    if (is_opaque() || other.is_opaque()) return false;  // opaque is never same-origin
    return scheme == other.scheme && host == other.host && port == other.port;
}

std::string Origin::serialize() const noexcept {
    if (is_opaque()) return "null";
    std::string out = scheme + "://" + host;
    if (port != 0 && port != default_port(scheme)) out += ":" + std::to_string(port);
    return out;
}

Origin Origin::opaque() {
    static std::atomic<uint64_t> counter{0};
    Origin o;
    o.scheme = "null";
    o.host = "opaque-" + std::to_string(counter.fetch_add(1));
    o.port = 0;
    return o;
}

Origin Origin::parse(std::string_view str) {
    std::string url(str);
    auto sep = url.find("://");
    if (sep == std::string::npos) return opaque();

    std::string scheme = to_lower(url.substr(0, sep));
    if (scheme.empty() || !scheme_has_authority(scheme)) return opaque();

    std::string rest = url.substr(sep + 3);
    // authority ends at the first '/', '?' or '#'
    size_t auth_end = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, auth_end);

    // strip userinfo
    if (auto at = authority.rfind('@'); at != std::string::npos)
        authority = authority.substr(at + 1);

    std::string host = authority;
    uint16_t port = default_port(scheme);
    if (auto colon = authority.rfind(':'); colon != std::string::npos &&
        authority.find(']', colon) == std::string::npos) {  // not inside IPv6 [..]
        host = authority.substr(0, colon);
        std::string ps = authority.substr(colon + 1);
        if (!ps.empty() && std::all_of(ps.begin(), ps.end(),
                                       [](unsigned char c) { return std::isdigit(c); })) {
            long v = std::stol(ps);
            port = (v >= 0 && v <= 65535) ? static_cast<uint16_t>(v) : 0;
        }
    }

    if (host.empty()) return opaque();

    Origin o;
    o.scheme = scheme;
    o.host = to_lower(host);
    o.port = port;
    return o;
}

} // namespace malibu::security
