// dom/document.cpp
// Document + DOMTree operations, including a Selectors-Level-4 subset matcher.

#include "malibu/dom/document.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace malibu::dom {
namespace {

// ASCII narrowing for selectors / tag / id / class values (these are ASCII in
// practice). Non-ASCII code units are passed through truncated.
std::string narrow(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char16_t c : s) out.push_back(static_cast<char>(c & 0xFF));
    return out;
}
std::u16string widen(std::string_view s) {
    return std::u16string(s.begin(), s.end());
}
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---- selector model ----
struct Compound {
    bool                     universal = false;
    std::string              tag;        // lowercased; empty = any
    std::string              id;         // empty = none
    std::vector<std::string> classes;
};
enum class Combinator { Descendant, Child };
struct CompoundStep {
    Combinator combinator;  // how this compound relates to the previous one
    Compound   compound;
};

// Parse a (single, comma-free) selector into a left-to-right step list.
std::vector<CompoundStep> parse_selector(const std::string& sel) {
    std::vector<CompoundStep> steps;
    size_t i = 0;
    Combinator pending = Combinator::Descendant;
    bool first = true;
    auto skip_ws = [&] { while (i < sel.size() && std::isspace((unsigned char)sel[i])) ++i; };

    while (i < sel.size()) {
        skip_ws();
        if (i >= sel.size()) break;
        if (sel[i] == '>') { pending = Combinator::Child; ++i; skip_ws(); }

        Compound c;
        bool consumed = false;
        while (i < sel.size() && !std::isspace((unsigned char)sel[i]) && sel[i] != '>') {
            char ch = sel[i];
            if (ch == '.') {
                ++i; std::string name;
                while (i < sel.size() && (std::isalnum((unsigned char)sel[i]) || sel[i] == '-' || sel[i] == '_')) name.push_back(sel[i++]);
                c.classes.push_back(name); consumed = true;
            } else if (ch == '#') {
                ++i; std::string name;
                while (i < sel.size() && (std::isalnum((unsigned char)sel[i]) || sel[i] == '-' || sel[i] == '_')) name.push_back(sel[i++]);
                c.id = name; consumed = true;
            } else if (ch == '*') {
                ++i; c.universal = true; consumed = true;
            } else if (std::isalnum((unsigned char)ch) || ch == '-' || ch == '_') {
                std::string name;
                while (i < sel.size() && (std::isalnum((unsigned char)sel[i]) || sel[i] == '-' || sel[i] == '_')) name.push_back(sel[i++]);
                c.tag = to_lower(name); consumed = true;
            } else {
                ++i;  // skip unknown token char
            }
        }
        if (consumed) {
            steps.push_back(CompoundStep{first ? Combinator::Descendant : pending, std::move(c)});
            first = false;
            pending = Combinator::Descendant;
        }
    }
    return steps;
}

} // namespace

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------
Document::Document() {
    root_ = create_node(kDocumentNode);
    // The document root is connected by definition.
    table_.set_state(root_, NodeState::AliveConnected);
}

NodeHandle Document::create_node(uint16_t node_type) {
    auto core = std::make_unique<NodeCore>();
    core->node_type = node_type;
    NodeCore* raw = core.get();
    cores_.push_back(std::move(core));
    NodeHandle h = table_.alloc(raw);  // alloc sets handle + AliveConnected
    return h;
}

NodeCore* Document::core(NodeHandle h) {
    NodeSlot* slot = table_.resolve(h);
    return slot ? slot->node : nullptr;
}
const NodeCore* Document::core(NodeHandle h) const {
    NodeSlot* slot = const_cast<NodeTable&>(table_).resolve(h);
    return slot ? slot->node : nullptr;
}

// ---------------------------------------------------------------------------
// DOMTree — creation
// ---------------------------------------------------------------------------
NodeHandle DOMTree::create_element(std::u16string_view tag_name) {
    NodeHandle h = doc_.create_node(kElementNode);
    if (NodeCore* c = doc_.core(h)) {
        std::string lower = to_lower(narrow(tag_name));
        c->tag_name = widen(lower);
    }
    // Freshly created, not yet inserted → detached.
    doc_.table().set_state(h, NodeState::AliveDetached);
    return h;
}

NodeHandle DOMTree::create_text_node(std::u16string_view data) {
    NodeHandle h = doc_.create_node(kTextNode);
    if (NodeCore* c = doc_.core(h)) c->text_content = std::u16string(data);
    doc_.table().set_state(h, NodeState::AliveDetached);
    return h;
}

NodeHandle DOMTree::create_document_fragment() {
    NodeHandle h = doc_.create_node(kDocumentFragmentNode);
    doc_.table().set_state(h, NodeState::AliveDetached);
    return h;
}

// ---------------------------------------------------------------------------
// DOMTree — connectivity
// ---------------------------------------------------------------------------
bool DOMTree::is_connected(NodeHandle h) const {
    // Connected iff a parent chain reaches the document root.
    NodeHandle cur = h;
    while (!cur.is_null()) {
        if (cur == doc_.root()) return true;
        const NodeCore* c = doc_.core(cur);
        if (!c) return false;
        cur = c->parent;
    }
    return false;
}

void DOMTree::update_connected_state(NodeHandle h, bool connected) {
    NodeCore* c = doc_.core(h);
    if (!c) return;
    NodeState s = doc_.table().state_of(h);
    if (s == NodeState::PendingDestroy || s == NodeState::Dead) return;
    doc_.table().set_state(h, connected ? NodeState::AliveConnected
                                        : NodeState::AliveDetached);
    for (NodeHandle child : c->children) update_connected_state(child, connected);
}

// ---------------------------------------------------------------------------
// DOMTree — structural mutations
// ---------------------------------------------------------------------------
DomError DOMTree::append_child(NodeHandle parent, NodeHandle child) {
    NodeState ps = doc_.table().state_of(parent);
    NodeState cs = doc_.table().state_of(child);
    if (ps == NodeState::Dead || cs == NodeState::Dead) return DomError::NotFound;
    if (ps == NodeState::PendingDestroy || cs == NodeState::PendingDestroy)
        return DomError::InvalidState;

    NodeCore* pc = doc_.core(parent);
    NodeCore* cc = doc_.core(child);
    if (!pc || !cc) return DomError::NotFound;

    // A DocumentFragment is never inserted itself — its children are moved into
    // the parent (DOM §4.2.3 "insert"). This is the common build-then-attach idiom.
    if (cc->node_type == kDocumentFragmentNode) {
        for (NodeHandle kid : std::vector<NodeHandle>(cc->children))
            append_child(parent, kid);
        return DomError::Ok;
    }

    // Reject hierarchy cycles (DOM §4.2.3): a node may not be inserted into
    // itself or into one of its own descendants — doing so would create a cycle
    // and infinite-loop the connected-state recomputation.
    for (NodeHandle p = parent; !p.is_null(); ) {
        if (p == child) return DomError::HierarchyRequest;
        NodeCore* pcore = doc_.core(p); p = pcore ? pcore->parent : NodeHandle::null_handle();
    }

    // Detach from a previous parent first.
    if (!cc->parent.is_null()) {
        if (NodeCore* old = doc_.core(cc->parent)) {
            std::erase(old->children, child);
        }
    }
    cc->parent = parent;
    pc->children.push_back(child);

    update_connected_state(child, is_connected(parent));
    return DomError::Ok;
}

DomError DOMTree::insert_before(NodeHandle parent, NodeHandle child, NodeHandle ref) {
    if (ref.is_null()) return append_child(parent, child);
    NodeState ps = doc_.table().state_of(parent);
    NodeState cs = doc_.table().state_of(child);
    if (ps == NodeState::Dead || cs == NodeState::Dead) return DomError::NotFound;
    if (ps == NodeState::PendingDestroy || cs == NodeState::PendingDestroy) return DomError::InvalidState;
    NodeCore* pc = doc_.core(parent);
    NodeCore* cc = doc_.core(child);
    if (!pc || !cc) return DomError::NotFound;
    if (std::find(pc->children.begin(), pc->children.end(), ref) == pc->children.end())
        return DomError::NotFound;
    // DocumentFragment: move its children before `ref` rather than the fragment.
    if (cc->node_type == kDocumentFragmentNode) {
        for (NodeHandle kid : std::vector<NodeHandle>(cc->children))
            insert_before(parent, kid, ref);
        return DomError::Ok;
    }
    // Reject hierarchy cycles (see append_child).
    for (NodeHandle p = parent; !p.is_null(); ) {
        if (p == child) return DomError::HierarchyRequest;
        NodeCore* pcore = doc_.core(p); p = pcore ? pcore->parent : NodeHandle::null_handle();
    }
    // Detach from a previous parent first (may shift positions in this parent).
    if (!cc->parent.is_null()) {
        if (NodeCore* old = doc_.core(cc->parent)) std::erase(old->children, child);
    }
    auto rit = std::find(pc->children.begin(), pc->children.end(), ref);
    cc->parent = parent;
    pc->children.insert(rit, child);
    update_connected_state(child, is_connected(parent));
    return DomError::Ok;
}

DomError DOMTree::remove_child(NodeHandle parent, NodeHandle child) {
    if (doc_.table().state_of(parent) == NodeState::Dead ||
        doc_.table().state_of(child) == NodeState::Dead)
        return DomError::NotFound;

    NodeCore* pc = doc_.core(parent);
    NodeCore* cc = doc_.core(child);
    if (!pc || !cc) return DomError::NotFound;

    auto it = std::find(pc->children.begin(), pc->children.end(), child);
    if (it == pc->children.end()) return DomError::HierarchyRequest;

    pc->children.erase(it);
    cc->parent = NodeHandle::null_handle();
    update_connected_state(child, false);
    return DomError::Ok;
}

DomError DOMTree::set_attribute(NodeHandle node, std::u16string_view name,
                                std::u16string_view value) {
    NodeState s = doc_.table().state_of(node);
    if (s == NodeState::Dead) return DomError::NotFound;
    if (s == NodeState::PendingDestroy) return DomError::InvalidState;
    NodeCore* c = doc_.core(node);
    if (!c) return DomError::NotFound;

    std::u16string key(name);
    auto& attrs = c->attributes;
    auto it = std::lower_bound(attrs.begin(), attrs.end(), key,
                               [](const auto& p, const std::u16string& k) { return p.first < k; });
    if (it != attrs.end() && it->first == key) it->second = std::u16string(value);
    else attrs.insert(it, {key, std::u16string(value)});
    return DomError::Ok;
}

std::optional<std::u16string> DOMTree::get_attribute(NodeHandle node,
                                                     std::u16string_view name) const {
    const NodeCore* c = doc_.core(node);
    if (!c) return std::nullopt;
    std::u16string key(name);
    auto it = std::lower_bound(c->attributes.begin(), c->attributes.end(), key,
                               [](const auto& p, const std::u16string& k) { return p.first < k; });
    if (it != c->attributes.end() && it->first == key) return it->second;
    return std::nullopt;
}

DomError DOMTree::set_text_content(NodeHandle node, std::u16string_view text) {
    NodeState s = doc_.table().state_of(node);
    if (s == NodeState::Dead) return DomError::NotFound;
    if (s == NodeState::PendingDestroy) return DomError::InvalidState;
    NodeCore* c = doc_.core(node);
    if (!c) return DomError::NotFound;

    if (c->node_type == kTextNode || c->node_type == kCommentNode) {
        c->text_content = std::u16string(text);   // CharacterData: its own data
    } else {
        // Replace all children with a single text node.
        for (NodeHandle child : c->children) update_connected_state(child, false);
        for (NodeHandle child : c->children) {
            if (NodeCore* cc = doc_.core(child)) cc->parent = NodeHandle::null_handle();
        }
        c->children.clear();
        NodeHandle tn = create_text_node(text);
        append_child(node, tn);
    }
    return DomError::Ok;
}

std::u16string DOMTree::text_content(NodeHandle node) const {
    const NodeCore* c = doc_.core(node);
    if (!c) return {};
    if (c->node_type == kTextNode) return c->text_content;
    std::u16string out;
    for (NodeHandle child : c->children) out += text_content(child);
    return out;
}

// ---------------------------------------------------------------------------
// DOMTree — selector matching
// ---------------------------------------------------------------------------
namespace {
bool matches_compound(const Document& doc, NodeHandle h, const Compound& comp) {
    const NodeCore* c = doc.core(h);
    if (!c || c->node_type != kElementNode) return false;
    if (!comp.universal && !comp.tag.empty()) {
        std::string tag(c->tag_name.begin(), c->tag_name.end());
        if (tag != comp.tag) return false;
    }
    if (!comp.id.empty()) {
        // id attribute
        bool ok = false;
        for (const auto& [k, v] : c->attributes) {
            if (k == u"id") { ok = (std::string(v.begin(), v.end()) == comp.id); break; }
        }
        if (!ok) return false;
    }
    if (!comp.classes.empty()) {
        std::string classlist;
        for (const auto& [k, v] : c->attributes) {
            if (k == u"class") { classlist = std::string(v.begin(), v.end()); break; }
        }
        std::vector<std::string> have;
        std::istringstream iss(classlist);
        std::string tok;
        while (iss >> tok) have.push_back(tok);
        for (const auto& want : comp.classes) {
            if (std::find(have.begin(), have.end(), want) == have.end()) return false;
        }
    }
    return true;
}

// Right-to-left match of a full step chain ending at `candidate`.
bool matches_chain(const Document& doc, NodeHandle candidate,
                   const std::vector<CompoundStep>& steps) {
    if (steps.empty()) return false;
    int si = static_cast<int>(steps.size()) - 1;
    if (!matches_compound(doc, candidate, steps[si].compound)) return false;

    NodeHandle cur = candidate;
    --si;
    while (si >= 0) {
        const CompoundStep& step = steps[si + 1];  // combinator relating step si → si+1
        const Compound& target = steps[si].compound;
        if (step.combinator == Combinator::Child) {
            const NodeCore* c = doc.core(cur);
            NodeHandle p = c ? c->parent : NodeHandle::null_handle();
            if (p.is_null() || !matches_compound(doc, p, target)) return false;
            cur = p;
            --si;
        } else {  // Descendant
            const NodeCore* c = doc.core(cur);
            NodeHandle p = c ? c->parent : NodeHandle::null_handle();
            bool found = false;
            while (!p.is_null()) {
                if (matches_compound(doc, p, target)) { found = true; cur = p; break; }
                const NodeCore* pc = doc.core(p);
                p = pc ? pc->parent : NodeHandle::null_handle();
            }
            if (!found) return false;
            --si;
        }
    }
    return true;
}

void collect_descendants(const Document& doc, NodeHandle h, std::vector<NodeHandle>& out) {
    const NodeCore* c = doc.core(h);
    if (!c) return;
    for (NodeHandle child : c->children) {
        out.push_back(child);
        collect_descendants(doc, child, out);
    }
}
} // namespace

NodeHandle DOMTree::query_selector(NodeHandle scope, std::u16string_view selector) {
    std::vector<CompoundStep> steps = parse_selector(narrow(selector));
    if (steps.empty()) return NodeHandle::null_handle();
    std::vector<NodeHandle> all;
    collect_descendants(doc_, scope, all);
    for (NodeHandle h : all) {
        if (matches_chain(doc_, h, steps)) return h;
    }
    return NodeHandle::null_handle();
}

void DOMTree::query_selector_all(NodeHandle scope, std::u16string_view selector,
                                 std::vector<NodeHandle>& out) {
    std::vector<CompoundStep> steps = parse_selector(narrow(selector));
    if (steps.empty()) return;
    std::vector<NodeHandle> all;
    collect_descendants(doc_, scope, all);  // pre-order = document order
    for (NodeHandle h : all) {
        if (matches_chain(doc_, h, steps)) out.push_back(h);
    }
}

// ---------------------------------------------------------------------------
// DOMTree — traversal
// ---------------------------------------------------------------------------
NodeHandle DOMTree::parent_node(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    return c ? c->parent : NodeHandle::null_handle();
}
std::vector<NodeHandle> DOMTree::child_nodes(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    return c ? c->children : std::vector<NodeHandle>{};
}
NodeHandle DOMTree::first_child(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    return (c && !c->children.empty()) ? c->children.front() : NodeHandle::null_handle();
}
NodeHandle DOMTree::last_child(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    return (c && !c->children.empty()) ? c->children.back() : NodeHandle::null_handle();
}
NodeHandle DOMTree::next_sibling(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    if (!c || c->parent.is_null()) return NodeHandle::null_handle();
    const NodeCore* p = doc_.core(c->parent);
    if (!p) return NodeHandle::null_handle();
    auto it = std::find(p->children.begin(), p->children.end(), h);
    if (it == p->children.end() || std::next(it) == p->children.end())
        return NodeHandle::null_handle();
    return *std::next(it);
}
NodeHandle DOMTree::previous_sibling(NodeHandle h) const {
    const NodeCore* c = doc_.core(h);
    if (!c || c->parent.is_null()) return NodeHandle::null_handle();
    const NodeCore* p = doc_.core(c->parent);
    if (!p) return NodeHandle::null_handle();
    auto it = std::find(p->children.begin(), p->children.end(), h);
    if (it == p->children.begin() || it == p->children.end())
        return NodeHandle::null_handle();
    return *std::prev(it);
}

} // namespace malibu::dom
