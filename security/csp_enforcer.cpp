// security/csp_enforcer.cpp
// CSP parsing + source-expression matching (subset of CSP Level 3).

#include "malibu/security/csp_enforcer.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace malibu::security {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

bool host_matches(std::string_view pattern, const std::string& host) {
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        std::string suffix(pattern.substr(1));  // ".example.com"
        return host.size() > suffix.size() &&
               host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return pattern == host;
}

}  // namespace

void CspEnforcer::set_policy(std::string_view policy) {
    has_policy_ = true;
    std::string p(policy);
    std::stringstream ss(p);
    std::string directive;
    while (std::getline(ss, directive, ';')) {
        auto tokens = split_ws(directive);
        if (tokens.empty()) continue;
        std::string name = to_lower(tokens[0]);
        std::vector<std::string> srcs(tokens.begin() + 1, tokens.end());

        if (name == "default-src")        policy_.default_src = srcs;
        else if (name == "script-src")    policy_.script_src = srcs;
        else if (name == "style-src")     policy_.style_src = srcs;
        else if (name == "img-src")       policy_.img_src = srcs;
        else if (name == "connect-src")   policy_.connect_src = srcs;
        else if (name == "font-src")      policy_.font_src = srcs;
        else if (name == "frame-src")     policy_.frame_src = srcs;
        else if (name == "media-src")     policy_.media_src = srcs;
        else if (name == "object-src")    policy_.object_src = srcs;
        else if (name == "child-src")     policy_.child_src = srcs;
        else if (name == "worker-src")    policy_.worker_src = srcs;
        else if (name == "manifest-src")  policy_.manifest_src = srcs;
        else if (name == "prefetch-src")  policy_.prefetch_src = srcs;
        else if (name == "form-action")   policy_.form_action = srcs;
        else if (name == "frame-ancestors") policy_.frame_ancestors = srcs;
        else if (name == "navigate-to")   policy_.navigate_to = srcs;
        else if (name == "report-uri")    { if (!srcs.empty()) policy_.report_uri = srcs[0]; }
        else if (name == "report-to")     { if (!srcs.empty()) policy_.report_to = srcs[0]; }
    }
}

const std::vector<std::string>& CspEnforcer::list_for(Directive d) const {
    switch (d) {
        case Directive::DefaultSrc:    return policy_.default_src;
        case Directive::ScriptSrc:     return policy_.script_src;
        case Directive::StyleSrc:      return policy_.style_src;
        case Directive::ImgSrc:        return policy_.img_src;
        case Directive::ConnectSrc:    return policy_.connect_src;
        case Directive::FontSrc:       return policy_.font_src;
        case Directive::FrameSrc:      return policy_.frame_src;
        case Directive::MediaSrc:      return policy_.media_src;
        case Directive::ObjectSrc:     return policy_.object_src;
        case Directive::ChildSrc:      return policy_.child_src;
        case Directive::WorkerSrc:     return policy_.worker_src;
        case Directive::ManifestSrc:   return policy_.manifest_src;
        case Directive::PrefetchSrc:   return policy_.prefetch_src;
        case Directive::FormAction:    return policy_.form_action;
        case Directive::FrameAncestors:return policy_.frame_ancestors;
        case Directive::NavigateTo:    return policy_.navigate_to;
        default:                       return policy_.default_src;
    }
}

bool CspEnforcer::source_list_allows(const std::vector<std::string>& list,
                                     std::string_view url) const {
    // 'none' anywhere blocks everything.
    for (const auto& s : list) if (s == "'none'") return false;

    Origin target = Origin::parse(url);
    for (const auto& src : list) {
        if (src == "*") {
            if (!target.is_opaque()) return true;        // any network origin
            continue;
        }
        if (src == "'self'") {
            if (!document_origin_.is_opaque() && document_origin_.same_origin(target)) return true;
            continue;
        }
        if (src == "'unsafe-inline'" || src == "'unsafe-eval'") continue;  // not URL sources

        std::string s = src;
        // scheme-source: "https:"
        if (s.size() >= 2 && s.back() == ':' && s.find("://") == std::string::npos) {
            std::string scheme = to_lower(s.substr(0, s.size() - 1));
            if (target.scheme == scheme) return true;
            continue;
        }
        // host/origin-source, optionally with scheme and port
        Origin src_origin;
        std::string host_part;
        uint16_t port_part = 0;
        bool have_scheme = false, have_port = false;
        if (auto pos = s.find("://"); pos != std::string::npos) {
            src_origin = Origin::parse(s);
            have_scheme = true;
            if (!src_origin.is_opaque()) {
                if (src_origin.scheme != target.scheme) continue;
                if (!host_matches(src_origin.host, target.host)) continue;
                if (s.rfind(':') > pos + 2 && src_origin.port != target.port) continue;
                return true;
            }
            continue;
        }
        // scheme-less host[:port]
        host_part = s;
        if (auto colon = s.rfind(':'); colon != std::string::npos) {
            std::string ps = s.substr(colon + 1);
            if (!ps.empty() && std::all_of(ps.begin(), ps.end(),
                                           [](unsigned char c){ return std::isdigit(c); })) {
                host_part = s.substr(0, colon);
                port_part = static_cast<uint16_t>(std::stol(ps));
                have_port = true;
            }
        }
        (void)have_scheme;
        if (host_matches(host_part, target.host) &&
            (!have_port || port_part == target.port)) {
            return true;
        }
    }
    return false;
}

bool CspEnforcer::allows(Directive directive, std::string_view url) const {
    if (!has_policy_) return true;  // no CSP header → all loads permitted (Req 14.2)

    const std::vector<std::string>* list = &list_for(directive);
    if (list->empty() && directive != Directive::DefaultSrc) {
        list = &policy_.default_src;  // fetch directives fall back to default-src
    }
    if (list->empty()) return true;  // directive unspecified and no default-src
    return source_list_allows(*list, url);
}

void CspEnforcer::report_violation(Directive directive, std::string_view blocked_url,
                                   std::string_view document_url) {
    violations_.push_back(Violation{directive, std::string(blocked_url), std::string(document_url)});
    MALIBU_LOG(WARNING, "csp",
               "CSP violation: blocked " + std::string(blocked_url) +
                   " (document " + std::string(document_url) + ")");
    // A real report-uri / report-to POST is dispatched asynchronously by the
    // Network Engine; it never blocks or alters the (already-made) decision.
}

} // namespace malibu::security
