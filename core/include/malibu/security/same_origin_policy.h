#pragma once
// core/include/malibu/security/same_origin_policy.h
// Same-Origin Policy enforcement (Task 13 / Requirement 14.1, 14.4).

#include <stdexcept>
#include <string>
#include "origin.h"

namespace malibu::security {

class JSRealm;  // opaque; cross-realm isolation is checked by realm identity

// Represents a SecurityError DOMException surfaced to the accessing script.
class SecurityError : public std::runtime_error {
public:
    explicit SecurityError(const std::string& msg) : std::runtime_error(msg) {}
};

class SameOriginPolicy {
public:
    // Throws SecurityError if accessor and resource differ without CORS /
    // document.domain agreement.
    static void check(const Origin& accessor, const Origin& resource, JSRealm& realm);

    // Throws SecurityError if a and b are not same-origin.
    static void check_same_origin(const Origin& a, const Origin& b);

    // Non-throwing query.
    [[nodiscard]] static bool is_allowed(const Origin& accessor, const Origin& resource) noexcept;
};

} // namespace malibu::security
