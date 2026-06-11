#pragma once
// core/include/malibu/css/style/style_resolver.h
// Resolves ComputedStyle for every element of a document: matching, cascade,
// inheritance, var() substitution, display:none exclusion, and fallbacks
// (Task 11 / Requirement 6.3-6.9).

#include <cstdint>
#include <deque>
#include <vector>

#include "malibu/types.h"
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/selector/selector.h"
#include "malibu/css/cascade/cascade.h"
#include "malibu/css/computed_style/computed_style.h"

namespace malibu::dom { class Document; }

namespace malibu::css {

// The built-in user-agent stylesheet (element display defaults, etc.). Load it
// at Origin::UserAgent before author sheets.
[[nodiscard]] std::u16string user_agent_css();

class StyleResolver {
public:
    // Adds a stylesheet at the given cascade origin (selectors are pre-parsed).
    void add_stylesheet(const StyleSheet& sheet, Origin origin);

    // Viewport size used to evaluate @media queries (set before resolve()).
    void set_viewport(float width, float height) { viewport_w_ = width; viewport_h_ = height; }

    // Resolves styles for the whole document, storing a ComputedStyle on each
    // element's NodeCore (owned by this resolver).
    void resolve(malibu::dom::Document& doc);

    // Re-resolves only the subtree rooted at `node` (used by invalidation),
    // inheriting from the parent element's already-computed style.
    void resolve_subtree(malibu::dom::Document& doc, malibu::NodeHandle node);

    [[nodiscard]] const ComputedStyle* style_for(malibu::dom::Document& doc,
                                                 malibu::NodeHandle node) const;

    [[nodiscard]] size_t rule_count() const noexcept { return rules_.size(); }

private:
    struct MatchableRule {
        ComplexSelector selector;
        const Rule*     rule = nullptr;
        Origin          origin = Origin::Author;
        uint32_t        order = 0;
    };

    void resolve_element(malibu::dom::Document& doc, malibu::NodeHandle node,
                         const ComputedStyle& parent);
    ComputedStyle compute(malibu::dom::Document& doc, malibu::NodeHandle node,
                          const ComputedStyle& parent);

    std::vector<MatchableRule>     rules_;
    std::deque<StyleSheet>         sheets_;   // stable storage for rules_ pointers
    std::deque<ComputedStyle>      pool_;     // owns computed styles
    uint32_t                       order_counter_ = 0;
    float                          viewport_w_ = 1024.0f;
    float                          viewport_h_ = 768.0f;
};

} // namespace malibu::css
