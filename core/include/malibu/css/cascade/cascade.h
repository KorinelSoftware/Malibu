#pragma once
// core/include/malibu/css/cascade/cascade.h
// Cascade origins and a single resolved cascade entry (Task 11 / Req 6.3).

#include <cstdint>
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/selector/selector.h"

namespace malibu::css {

// Cascade origin, lowest-to-highest precedence.
enum class Origin : uint8_t { UserAgent = 0, User = 1, Author = 2, Inline = 3 };

// A declaration that matched an element, with the data needed to order it in
// the cascade: (origin, importance, specificity, source order).
struct CascadeEntry {
    const Declaration* decl = nullptr;
    Origin             origin = Origin::Author;
    Specificity        specificity;
    uint32_t           order = 0;   // document/source order

    // Strict-weak ordering: returns true if `this` wins over `o`.
    [[nodiscard]] bool wins_over(const CascadeEntry& o) const noexcept {
        if (decl->important != o.decl->important) return decl->important;  // !important wins
        if (origin != o.origin) return origin > o.origin;
        if (!(specificity == o.specificity)) return o.specificity < specificity;
        return order >= o.order;  // later source order wins ties
    }
};

} // namespace malibu::css
