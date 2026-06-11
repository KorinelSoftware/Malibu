#pragma once
// core/include/malibu/dom/dom_command_buffer.h
// Batched DOM mutations (Task 8).
//
// Mutations are queued and not visible in DOM reads until the next flush.
// A layout-observable read must flush first. On a flush failure the buffer
// halts, preserves the remaining (unapplied) commands in order, and reports an
// error; already-applied mutations are not rolled back.

#include <cstdint>
#include <deque>
#include <string>
#include "../types.h"
#include "node_table.h"
#include "document.h"

namespace malibu::dom {

class UnifiedObjectGraphAdapter;  // optional; see flush()

// Abstract layout-invalidation sink so the buffer does not depend on the
// concrete Layout Engine. flush() calls mark_dirty for every mutated target
// (the Layout Engine is responsible for walking ancestor chains).
class LayoutInvalidator {
public:
    virtual ~LayoutInvalidator() = default;
    virtual void mark_dirty(NodeHandle target) = 0;
};

enum class DomCommandType : uint32_t {
    AppendChild,      // target = parent, arg_node = child
    RemoveChild,      // target = parent, arg_node = child
    Remove,           // target = node (removed from its parent)
    SetAttribute,     // target = node, key, value
    SetTextContent,   // target = node, value
};

struct DomCommand {
    DomCommandType  type;
    NodeHandle      target;
    std::u16string  key;
    std::u16string  value;
    NodeHandle      arg_node = NodeHandle::null_handle();
};

struct FlushResult {
    bool     success            = true;
    uint32_t applied_count      = 0;
    uint32_t first_failed_index = 0;   // valid only when !success
    int32_t  error_code         = 0;   // DomError as int when !success
};

class DomCommandBuffer {
public:
    explicit DomCommandBuffer(NodeTable& table) : table_(table) {}

    // Pins the target (and arg_node) in the NodeTable, then enqueues.
    void push(DomCommand cmd);

    // Applies all queued commands in order against `tree`. Notifies `layout`
    // (if non-null) of each mutated target. Empty buffer → success, no layout
    // notification. Returns on first failure with remaining commands preserved.
    FlushResult flush(DOMTree& tree, LayoutInvalidator* layout = nullptr);

    // Layout-observable reads call this before returning a value.
    FlushResult flush_for_read(DOMTree& tree, LayoutInvalidator* layout = nullptr) {
        return flush(tree, layout);
    }

    [[nodiscard]] bool   is_empty() const noexcept { return commands_.empty(); }
    [[nodiscard]] size_t size()     const noexcept { return commands_.size(); }

private:
    void pin(NodeHandle h);
    void unpin(NodeHandle h);

    NodeTable&             table_;
    std::deque<DomCommand> commands_;
};

} // namespace malibu::dom
