#pragma once
// core/include/malibu/dom/document.h
// Document + DOM tree operations (Task 9).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "node_table.h"

namespace malibu::dom {

// Result of a structural DOM mutation. Mutations on Dead/PendingDestroy nodes
// are rejected with InvalidState and leave the tree unchanged (Req 3.10).
enum class DomError : uint8_t {
    Ok = 0,
    InvalidState,    // target is PendingDestroy / Dead
    NotFound,        // handle does not resolve
    HierarchyRequest // e.g. child is not a child of parent
};

// Owns the NodeTable and the backing storage for all NodeCore objects created
// for this document. NodeCore instances live until the Document is destroyed;
// freeing a node detaches it from its table slot but does not invalidate other
// raw NodeCore pointers.
class Document {
public:
    Document();

    NodeTable&       table()       noexcept { return table_; }
    const NodeTable& table() const noexcept { return table_; }
    NodeHandle       root() const  noexcept { return root_; }

    // Allocates a fresh, owned NodeCore and registers it in the NodeTable.
    NodeHandle create_node(uint16_t node_type);

    // Resolves a handle to its NodeCore, or nullptr if not alive.
    NodeCore*       core(NodeHandle h);
    const NodeCore* core(NodeHandle h) const;

private:
    NodeTable                               table_;
    NodeHandle                              root_;
    std::vector<std::unique_ptr<NodeCore>>  cores_;  // owns NodeCore objects
};

class DOMTree {
public:
    explicit DOMTree(Document& doc) : doc_(doc) {}

    NodeHandle create_element(std::u16string_view tag_name);
    NodeHandle create_text_node(std::u16string_view data);
    NodeHandle create_document_fragment();

    DomError append_child(NodeHandle parent, NodeHandle child);
    DomError insert_before(NodeHandle parent, NodeHandle child, NodeHandle ref);  // ref==null → append
    DomError remove_child(NodeHandle parent, NodeHandle child);
    DomError set_attribute(NodeHandle node, std::u16string_view name,
                           std::u16string_view value);
    std::optional<std::u16string> get_attribute(NodeHandle node,
                                                 std::u16string_view name) const;

    DomError set_text_content(NodeHandle node, std::u16string_view text);
    std::u16string text_content(NodeHandle node) const;  // concatenated descendant text

    NodeHandle query_selector(NodeHandle scope, std::u16string_view selector);
    void       query_selector_all(NodeHandle scope, std::u16string_view selector,
                                  std::vector<NodeHandle>& out);

    // Traversal (WHATWG DOM)
    NodeHandle              parent_node(NodeHandle h) const;
    std::vector<NodeHandle> child_nodes(NodeHandle h) const;
    NodeHandle              first_child(NodeHandle h) const;
    NodeHandle              last_child(NodeHandle h) const;
    NodeHandle              next_sibling(NodeHandle h) const;
    NodeHandle              previous_sibling(NodeHandle h) const;

    Document& document() noexcept { return doc_; }

    // True when `h` is reachable from the document root (Node.isConnected).
    [[nodiscard]] bool is_connected(NodeHandle h) const;

private:
    // Recomputes connected/detached state for `h` and its subtree based on
    // whether it is reachable from the document root.
    void update_connected_state(NodeHandle h, bool connected);

    Document& doc_;
};

} // namespace malibu::dom
