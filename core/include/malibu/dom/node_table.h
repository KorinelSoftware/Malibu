#pragma once
// core/include/malibu/dom/node_table.h
// NodeHandle slab allocator.

#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <mutex>
#include "../types.h"

namespace malibu::dom {

// DOM node type codes per the WHATWG DOM specification (subset).
inline constexpr uint16_t kElementNode          = 1;
inline constexpr uint16_t kTextNode             = 3;
inline constexpr uint16_t kCommentNode          = 8;
inline constexpr uint16_t kDocumentNode         = 9;
inline constexpr uint16_t kDocumentFragmentNode = 11;

enum class NodeState : uint8_t {
    AliveConnected  = 0,
    AliveDetached   = 1,
    PendingDestroy  = 2,
    Dead            = 3,
};

struct NodeCore;

struct NodeSlot {
    uint32_t generation;
    NodeState state;
    uint8_t pin_count;
    uint16_t node_type;
    NodeCore* node;
};

class NodeTable {
public:
    NodeHandle alloc(NodeCore* node);
    void free(NodeHandle h);
    NodeSlot* resolve(NodeHandle h);
    void pin(NodeHandle h);
    void unpin(NodeHandle h);
    NodeState state_of(NodeHandle h) const;
    void set_state(NodeHandle h, NodeState s);
private:
    std::vector<NodeSlot> slots_;
    std::vector<uint32_t> free_list_;
    mutable std::mutex mu_;
};

// Flat sorted (by name) attribute list — cache-friendly for small element
// attribute sets, which dominate real pages.
using AttrMap = std::vector<std::pair<std::u16string, std::u16string>>;

struct NodeCore {
    NodeHandle              handle{};
    uint16_t                node_type = 0;
    NodeHandle              parent = NodeHandle::null_handle();
    std::vector<NodeHandle> children;

    std::u16string          tag_name;       // ELEMENT_NODE only (lowercased)
    AttrMap                 attributes;      // ELEMENT_NODE only
    std::u16string          text_content;    // TEXT_NODE / COMMENT_NODE

    malibu::JSObjectHandle    wrapper{};       // null until first JS access
    malibu::css::ComputedStyle* computed_style = nullptr;  // owned by CSS Engine
    malibu::css::ComputedStyle* before_style   = nullptr;  // generated box, not DOM
    malibu::css::ComputedStyle* after_style    = nullptr;  // generated box, not DOM
    malibu::layout::LayoutBox*  layout_box     = nullptr;  // owned by Layout Engine

    // Dynamic interaction state (drives :hover/:focus/:active selectors). Set by
    // the host/View on input; read by the selector matcher.
    bool                    hovered = false;
    bool                    focused = false;
    bool                    active  = false;
};

} // namespace malibu::dom
