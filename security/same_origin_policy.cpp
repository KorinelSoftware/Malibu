// security/same_origin_policy.cpp
// Same-Origin Policy checks.

#include "malibu/security/same_origin_policy.h"

namespace malibu::security {

bool SameOriginPolicy::is_allowed(const Origin& accessor, const Origin& resource) noexcept {
    return accessor.same_origin(resource);
}

void SameOriginPolicy::check(const Origin& accessor, const Origin& resource, JSRealm&) {
    if (!accessor.same_origin(resource)) {
        throw SecurityError("Blocked a cross-origin access from " + accessor.serialize() +
                            " to " + resource.serialize() + " by the same-origin policy");
    }
}

void SameOriginPolicy::check_same_origin(const Origin& a, const Origin& b) {
    if (!a.same_origin(b)) {
        throw SecurityError("Origins are not same-origin: " + a.serialize() + " vs " +
                            b.serialize());
    }
}

} // namespace malibu::security
