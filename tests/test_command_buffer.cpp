// tests/test_command_buffer.cpp
// Task 8: ordering, empty-flush no-notify, fail-stop preservation, pinning.

#include <gtest/gtest.h>
#include "malibu/dom/document.h"
#include "malibu/dom/dom_command_buffer.h"

using namespace malibu::dom;
using malibu::NodeHandle;

namespace {
struct CountingInvalidator : LayoutInvalidator {
    std::vector<NodeHandle> dirtied;
    void mark_dirty(NodeHandle h) override { dirtied.push_back(h); }
};
}  // namespace

TEST(DomCommandBuffer, AppliesInOrder) {
    Document doc;
    DOMTree tree(doc);
    DomCommandBuffer buf(doc.table());

    NodeHandle a = tree.create_element(u"a");
    NodeHandle b = tree.create_element(u"b");
    NodeHandle c = tree.create_element(u"c");

    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, a});
    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, b});
    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, c});

    // Not visible before flush.
    EXPECT_TRUE(tree.child_nodes(doc.root()).empty());

    CountingInvalidator inv;
    FlushResult r = buf.flush(tree, &inv);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.applied_count, 3u);

    auto kids = tree.child_nodes(doc.root());
    ASSERT_EQ(kids.size(), 3u);
    EXPECT_EQ(kids[0], a);
    EXPECT_EQ(kids[1], b);
    EXPECT_EQ(kids[2], c);
    EXPECT_EQ(inv.dirtied.size(), 3u);
    EXPECT_TRUE(buf.is_empty());
}

TEST(DomCommandBuffer, EmptyFlushDoesNotNotifyLayout) {
    Document doc;
    DOMTree tree(doc);
    DomCommandBuffer buf(doc.table());
    CountingInvalidator inv;
    FlushResult r = buf.flush(tree, &inv);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.applied_count, 0u);
    EXPECT_TRUE(inv.dirtied.empty());
}

TEST(DomCommandBuffer, MidFlushFailurePreservesRemaining) {
    Document doc;
    DOMTree tree(doc);
    DomCommandBuffer buf(doc.table());

    NodeHandle ok1 = tree.create_element(u"x");
    NodeHandle dead = tree.create_element(u"y");
    NodeHandle ok2 = tree.create_element(u"z");

    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, ok1});
    buf.push({DomCommandType::SetAttribute, dead, u"id", u"bad", NodeHandle::null_handle()});
    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, ok2});

    // Kill the middle target so its command fails at flush.
    doc.table().set_state(dead, NodeState::Dead);

    FlushResult r = buf.flush(tree);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.applied_count, 1u);
    EXPECT_EQ(r.first_failed_index, 1u);

    // First mutation applied (not rolled back); two commands preserved in order.
    EXPECT_EQ(tree.child_nodes(doc.root()).size(), 1u);
    EXPECT_EQ(buf.size(), 2u);
}

TEST(DomCommandBuffer, PendingCommandPinsTarget) {
    Document doc;
    DOMTree tree(doc);
    DomCommandBuffer buf(doc.table());

    NodeHandle el = tree.create_element(u"div");
    buf.push({DomCommandType::SetAttribute, el, u"id", u"app", NodeHandle::null_handle()});

    // Pinned: free must be refused while the command is pending.
    doc.table().free(el);
    EXPECT_NE(doc.table().resolve(el), nullptr);

    // After flush the pin is released.
    buf.flush(tree);
    doc.table().free(el);
    EXPECT_EQ(doc.table().resolve(el), nullptr);
}

TEST(DomCommandBuffer, FlushForReadAppliesPending) {
    Document doc;
    DOMTree tree(doc);
    DomCommandBuffer buf(doc.table());
    NodeHandle el = tree.create_element(u"p");
    buf.push({DomCommandType::AppendChild, doc.root(), {}, {}, el});
    EXPECT_TRUE(tree.child_nodes(doc.root()).empty());
    buf.flush_for_read(tree);
    EXPECT_EQ(tree.child_nodes(doc.root()).size(), 1u);
}
