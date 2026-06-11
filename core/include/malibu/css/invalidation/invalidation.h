#pragma once
// core/include/malibu/css/invalidation/invalidation.h
// Style invalidation: tracks dirty nodes and recomputes their subtrees before
// the next layout pass (Task 11 / Requirement 6.5).

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "malibu/types.h"
#include "malibu/css/style/style_resolver.h"

namespace malibu::dom { class Document; }

namespace malibu::css {

enum class InvalidationReason : uint8_t {
    AttributeChanged,
    ClassChanged,
    IdChanged,
    StyleAttributeChanged,
    StructureChanged,
    MediaQueryChanged,
};

class StyleInvalidator {
public:
    void mark_dirty(malibu::NodeHandle h, InvalidationReason reason);
    [[nodiscard]] bool   is_dirty(malibu::NodeHandle h) const;
    [[nodiscard]] size_t dirty_count() const noexcept { return dirty_.size(); }
    void clear() noexcept { dirty_.clear(); }

    // Recomputes ComputedStyle for every dirty node's subtree, then clears the
    // dirty set. Subtree-targeted so unaffected nodes keep their styles.
    void recompute(malibu::dom::Document& doc, StyleResolver& resolver);

private:
    static uint64_t key(malibu::NodeHandle h) {
        return (static_cast<uint64_t>(h.index) << 32) | h.generation;
    }
    std::unordered_set<uint64_t>            dirty_;
    std::vector<malibu::NodeHandle>         dirty_list_;
};

} // namespace malibu::css
