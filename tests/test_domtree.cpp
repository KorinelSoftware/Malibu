// tests/test_domtree.cpp
// Task 9: DOM tree operations, NodeState transitions, querySelector, traversal.

#include <gtest/gtest.h>
#include "malibu/dom/document.h"

using namespace malibu::dom;
using malibu::NodeHandle;

TEST(DOMTree, CreatedElementIsDetached) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle el = tree.create_element(u"div");
    EXPECT_EQ(doc.table().state_of(el), NodeState::AliveDetached);
}

TEST(DOMTree, AppendConnectsSubtree) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle parent = tree.create_element(u"div");
    NodeHandle child  = tree.create_element(u"span");

    EXPECT_EQ(tree.append_child(parent, child), DomError::Ok);
    // parent not yet attached to root → both detached
    EXPECT_EQ(doc.table().state_of(parent), NodeState::AliveDetached);
    EXPECT_EQ(doc.table().state_of(child), NodeState::AliveDetached);

    // Attach parent to the document root → whole subtree becomes connected.
    EXPECT_EQ(tree.append_child(doc.root(), parent), DomError::Ok);
    EXPECT_EQ(doc.table().state_of(parent), NodeState::AliveConnected);
    EXPECT_EQ(doc.table().state_of(child), NodeState::AliveConnected);
}

TEST(DOMTree, RemoveDetachesSubtree) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle parent = tree.create_element(u"div");
    NodeHandle child  = tree.create_element(u"span");
    tree.append_child(doc.root(), parent);
    tree.append_child(parent, child);
    ASSERT_EQ(doc.table().state_of(child), NodeState::AliveConnected);

    EXPECT_EQ(tree.remove_child(doc.root(), parent), DomError::Ok);
    EXPECT_EQ(doc.table().state_of(parent), NodeState::AliveDetached);
    EXPECT_EQ(doc.table().state_of(child), NodeState::AliveDetached);
}

TEST(DOMTree, Traversal) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle ul = tree.create_element(u"ul");
    NodeHandle a = tree.create_element(u"li");
    NodeHandle b = tree.create_element(u"li");
    NodeHandle c = tree.create_element(u"li");
    tree.append_child(doc.root(), ul);
    tree.append_child(ul, a);
    tree.append_child(ul, b);
    tree.append_child(ul, c);

    EXPECT_EQ(tree.first_child(ul), a);
    EXPECT_EQ(tree.last_child(ul), c);
    EXPECT_EQ(tree.next_sibling(a), b);
    EXPECT_EQ(tree.previous_sibling(b), a);
    EXPECT_EQ(tree.next_sibling(c), NodeHandle::null_handle());
    EXPECT_EQ(tree.parent_node(b), ul);
    EXPECT_EQ(tree.child_nodes(ul).size(), 3u);
}

TEST(DOMTree, Attributes) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle el = tree.create_element(u"div");
    EXPECT_EQ(tree.set_attribute(el, u"id", u"app"), DomError::Ok);
    EXPECT_EQ(tree.set_attribute(el, u"class", u"box red"), DomError::Ok);
    auto id = tree.get_attribute(el, u"id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, std::u16string(u"app"));
    EXPECT_FALSE(tree.get_attribute(el, u"missing").has_value());
    // Overwrite
    tree.set_attribute(el, u"id", u"main");
    EXPECT_EQ(*tree.get_attribute(el, u"id"), std::u16string(u"main"));
}

TEST(DOMTree, QuerySelectorTypeClassId) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle body = tree.create_element(u"body");
    NodeHandle div  = tree.create_element(u"div");
    tree.set_attribute(div, u"id", u"app");
    tree.set_attribute(div, u"class", u"container");
    NodeHandle span = tree.create_element(u"span");
    tree.set_attribute(span, u"class", u"label");
    tree.append_child(doc.root(), body);
    tree.append_child(body, div);
    tree.append_child(div, span);

    EXPECT_EQ(tree.query_selector(doc.root(), u"div"), div);
    EXPECT_EQ(tree.query_selector(doc.root(), u"#app"), div);
    EXPECT_EQ(tree.query_selector(doc.root(), u".container"), div);
    EXPECT_EQ(tree.query_selector(doc.root(), u".label"), span);
    EXPECT_EQ(tree.query_selector(doc.root(), u"div#app.container"), div);
    EXPECT_EQ(tree.query_selector(doc.root(), u"nope"), NodeHandle::null_handle());
}

TEST(DOMTree, QuerySelectorCombinators) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle outer = tree.create_element(u"div");
    tree.set_attribute(outer, u"class", u"outer");
    NodeHandle mid = tree.create_element(u"section");
    NodeHandle inner = tree.create_element(u"span");
    tree.set_attribute(inner, u"class", u"target");
    tree.append_child(doc.root(), outer);
    tree.append_child(outer, mid);
    tree.append_child(mid, inner);

    // descendant
    EXPECT_EQ(tree.query_selector(doc.root(), u".outer .target"), inner);
    // child combinator should NOT match (span is a grandchild of .outer)
    EXPECT_EQ(tree.query_selector(doc.root(), u".outer > .target"), NodeHandle::null_handle());
    // direct child combinator that does match
    EXPECT_EQ(tree.query_selector(doc.root(), u"section > span"), inner);
}

TEST(DOMTree, QuerySelectorAllDocumentOrder) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle root = tree.create_element(u"div");
    tree.append_child(doc.root(), root);
    std::vector<NodeHandle> created;
    for (int i = 0; i < 4; ++i) {
        NodeHandle p = tree.create_element(u"p");
        tree.append_child(root, p);
        created.push_back(p);
    }
    std::vector<NodeHandle> out;
    tree.query_selector_all(doc.root(), u"p", out);
    ASSERT_EQ(out.size(), 4u);
    for (size_t i = 0; i < created.size(); ++i) EXPECT_EQ(out[i], created[i]);
}

TEST(DOMTree, MutationOnDeadNodeRejected) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle el = tree.create_element(u"div");
    doc.table().set_state(el, NodeState::Dead);
    EXPECT_EQ(tree.set_attribute(el, u"id", u"x"), DomError::NotFound);

    NodeHandle pend = tree.create_element(u"div");
    doc.table().set_state(pend, NodeState::PendingDestroy);
    EXPECT_EQ(tree.set_attribute(pend, u"id", u"x"), DomError::InvalidState);
}

TEST(DOMTree, TextContent) {
    Document doc;
    DOMTree tree(doc);
    NodeHandle p = tree.create_element(u"p");
    tree.append_child(doc.root(), p);
    EXPECT_EQ(tree.set_text_content(p, u"Hola Malibu"), DomError::Ok);
    EXPECT_EQ(tree.text_content(p), std::u16string(u"Hola Malibu"));
}
