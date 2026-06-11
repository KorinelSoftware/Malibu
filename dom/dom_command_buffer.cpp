// dom/dom_command_buffer.cpp
// Batched DOM mutation application with pinning and ordered, fail-stop flush.

#include "malibu/dom/dom_command_buffer.h"
#include "malibu/diagnostics/diagnostic_log.h"

namespace malibu::dom {

void DomCommandBuffer::pin(NodeHandle h) {
    if (!h.is_null()) table_.pin(h);
}
void DomCommandBuffer::unpin(NodeHandle h) {
    if (!h.is_null()) table_.unpin(h);
}

void DomCommandBuffer::push(DomCommand cmd) {
    pin(cmd.target);
    pin(cmd.arg_node);
    commands_.push_back(std::move(cmd));
}

FlushResult DomCommandBuffer::flush(DOMTree& tree, LayoutInvalidator* layout) {
    FlushResult result;
    if (commands_.empty()) {
        // Empty flush: do not notify layout (Req 5.7).
        result.success = true;
        return result;
    }

    uint32_t index = 0;
    while (!commands_.empty()) {
        DomCommand cmd = commands_.front();

        DomError err = DomError::Ok;
        switch (cmd.type) {
            case DomCommandType::AppendChild:
                err = tree.append_child(cmd.target, cmd.arg_node);
                break;
            case DomCommandType::RemoveChild:
                err = tree.remove_child(cmd.target, cmd.arg_node);
                break;
            case DomCommandType::Remove: {
                NodeHandle parent = tree.parent_node(cmd.target);
                err = parent.is_null() ? DomError::Ok
                                       : tree.remove_child(parent, cmd.target);
                break;
            }
            case DomCommandType::SetAttribute:
                err = tree.set_attribute(cmd.target, cmd.key, cmd.value);
                break;
            case DomCommandType::SetTextContent:
                err = tree.set_text_content(cmd.target, cmd.value);
                break;
        }

        if (err != DomError::Ok) {
            // Halt: preserve remaining commands (still pinned), report failure.
            MALIBU_LOG(WARNING, "dom",
                       std::string("DomCommandBuffer flush failed at index ") +
                           std::to_string(index));
            result.success = false;
            result.applied_count = index;
            result.first_failed_index = index;
            result.error_code = static_cast<int32_t>(err);
            return result;
        }

        if (layout) layout->mark_dirty(cmd.target);

        // Command applied: unpin its referenced nodes and pop.
        unpin(cmd.target);
        unpin(cmd.arg_node);
        commands_.pop_front();
        ++index;
    }

    result.success = true;
    result.applied_count = index;
    return result;
}

} // namespace malibu::dom
