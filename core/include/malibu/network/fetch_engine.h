#pragma once
// core/include/malibu/network/fetch_engine.h
// WHATWG Fetch engine (Task 22): CORS enforcement, HTTP cache + revalidation,
// and a CSP connect-src pre-flight. Network I/O is delegated to a Transport so
// the engine is testable with a mock and OS-agnostic in core.

#include "malibu/network/fetch_types.h"
#include "malibu/network/http_cache.h"
#include "malibu/security/origin.h"

namespace malibu::security { class CspEnforcer; }

namespace malibu::network {

// Performs a single HTTP exchange. Returns false on a connection-level error
// (DNS failure, TCP refused, TLS handshake failure). The real implementation
// delegates to Platform::network_stack(); tests inject a mock.
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const FetchRequest& request, FetchResponse& out) = 0;
};

class FetchEngine {
public:
    explicit FetchEngine(Transport& transport) : transport_(transport) {}

    void set_csp(malibu::security::CspEnforcer* csp) { csp_ = csp; }

    // Fetches `request` initiated from `origin`. Applies CSP, cache, transport,
    // and CORS. A connection error or CORS failure yields ResponseType::Error
    // (which the JS binding turns into a rejected Promise with a TypeError).
    FetchResponse fetch(FetchRequest request, const malibu::security::Origin& origin);

    HttpCache& cache() noexcept { return cache_; }

private:
    static FetchResponse network_error(const std::string& url);

    Transport&                    transport_;
    HttpCache                     cache_;
    malibu::security::CspEnforcer* csp_ = nullptr;
};

} // namespace malibu::network
