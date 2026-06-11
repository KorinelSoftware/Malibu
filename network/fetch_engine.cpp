// network/fetch_engine.cpp
// WHATWG Fetch: CSP pre-flight, HTTP cache + revalidation, transport, CORS.

#include "malibu/network/fetch_engine.h"
#include "malibu/security/csp_enforcer.h"

namespace malibu::network {

using malibu::security::Origin;

FetchResponse FetchEngine::network_error(const std::string& url) {
    FetchResponse r;
    r.type = ResponseType::Error;
    r.ok = false;
    r.status = 0;
    r.url = url;
    return r;
}

FetchResponse FetchEngine::fetch(FetchRequest request, const Origin& origin) {
    Origin target = Origin::parse(request.url);
    bool cross_origin = !origin.same_origin(target);

    // (1) CSP connect-src check BEFORE any network connection (Req 11.6 / Prop 8).
    if (csp_ && csp_->has_policy() &&
        !csp_->allows(malibu::security::CspEnforcer::Directive::ConnectSrc, request.url)) {
        csp_->report_violation(malibu::security::CspEnforcer::Directive::ConnectSrc,
                               request.url, origin.serialize());
        return network_error(request.url);
    }

    // (2) HTTP cache lookup for cacheable GETs.
    const bool cacheable = (request.method == "GET");
    std::optional<CacheEntry> entry;
    if (cacheable && request.cache != CacheMode::NoStore && request.cache != CacheMode::Reload) {
        entry = cache_.lookup(request.url);
        if (entry) {
            bool must_revalidate = entry->no_cache || request.cache == CacheMode::NoCache;
            if (!must_revalidate && entry->has_max_age && entry->max_age > 0) {
                return entry->response;  // fresh: serve without touching the network
            }
            if (entry->has_validator()) {  // conditional revalidation
                if (!entry->etag.empty()) request.headers.set("If-None-Match", entry->etag);
                if (!entry->last_modified.empty()) request.headers.set("If-Modified-Since", entry->last_modified);
            }
        }
    }

    // (3) Transport. A connection-level failure is a network error.
    FetchResponse resp;
    if (!transport_.send(request, resp)) {
        return network_error(request.url);
    }

    // (4) 304 Not Modified → serve the stored representation.
    if (resp.status == 304 && entry) {
        return entry->response;
    }

    // (5) CORS check for cross-origin responses.
    if (cross_origin) {
        std::string allow = resp.headers.get("Access-Control-Allow-Origin");
        if (allow != "*" && allow != origin.serialize()) {
            return network_error(request.url);  // missing/incorrect CORS headers
        }
        resp.type = ResponseType::Cors;
    } else {
        resp.type = ResponseType::Basic;
    }
    resp.ok = (resp.status >= 200 && resp.status < 300);
    resp.url = request.url;

    // (6) Store cacheable responses.
    if (cacheable && request.cache != CacheMode::NoStore) {
        cache_.store(request.url, resp);
    }
    return resp;
}

} // namespace malibu::network
