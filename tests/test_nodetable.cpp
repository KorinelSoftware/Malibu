// tests/test_nodetable.cpp
// Unit tests for NodeTable.

#include <gtest/gtest.h>
#include "malibu/dom/node_table.h"

using malibu::dom::NodeTable;
using malibu::NodeHandle;
using malibu::dom::NodeState;
using malibu::dom::NodeSlot;
using malibu::dom::NodeCore;

TEST(NodeTableTest, AllocAndResolve) {
    NodeTable table;
    NodeCore node;
    node.node_type = 1;
    
    NodeHandle h = table.alloc(&node);
    EXPECT_FALSE(h.is_null());
    EXPECT_EQ(h.index, 0);
    EXPECT_EQ(h.generation, 0);
    
    NodeSlot* slot = table.resolve(h);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->state, NodeState::AliveConnected);
    EXPECT_EQ(slot->node, &node);
    EXPECT_EQ(slot->node_type, 1);
}

TEST(NodeTableTest, FreeAndRealloc) {
    NodeTable table;
    NodeCore node1;
    node1.node_type = 1;
    
    NodeHandle h1 = table.alloc(&node1);
    table.free(h1);
    
    // Resolve should fail for freed handle
    EXPECT_EQ(table.resolve(h1), nullptr);
    
    // Reallocate same slot
    NodeCore node2;
    node2.node_type = 2;
    NodeHandle h2 = table.alloc(&node2);
    
    EXPECT_EQ(h2.index, h1.index);
    EXPECT_EQ(h2.generation, h1.generation + 1);
    
    // Old handle should not resolve
    EXPECT_EQ(table.resolve(h1), nullptr);
    
    // New handle should resolve
    NodeSlot* slot = table.resolve(h2);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->generation, h2.generation);
    EXPECT_EQ(slot->node_type, 2);
}

TEST(NodeTableTest, PinUnpin) {
    NodeTable table;
    NodeCore node;
    
    NodeHandle h = table.alloc(&node);
    table.pin(h);
    table.pin(h);
    
    NodeSlot* slot = table.resolve(h);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pin_count, 2);
    
    table.unpin(h);
    EXPECT_EQ(table.resolve(h)->pin_count, 1);
    
    table.unpin(h);
    EXPECT_EQ(table.resolve(h)->pin_count, 0);
    
    // Free should work when unpinned
    table.free(h);
    EXPECT_EQ(table.resolve(h), nullptr);
}

TEST(NodeTableTest, FreePinnedFails) {
    NodeTable table;
    NodeCore node;
    
    NodeHandle h = table.alloc(&node);
    table.pin(h);
    table.free(h);  // Should not free
    
    NodeSlot* slot = table.resolve(h);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->pin_count, 1);
}

TEST(NodeTableTest, StateTransitions) {
    NodeTable table;
    NodeCore node;
    
    NodeHandle h = table.alloc(&node);
    EXPECT_EQ(table.state_of(h), NodeState::AliveConnected);
    
    table.set_state(h, NodeState::AliveDetached);
    EXPECT_EQ(table.state_of(h), NodeState::AliveDetached);
    
    table.set_state(h, NodeState::PendingDestroy);
    EXPECT_EQ(table.state_of(h), NodeState::PendingDestroy);
    
    table.set_state(h, NodeState::Dead);
    EXPECT_EQ(table.state_of(h), NodeState::Dead);
    
    // Dead node should not resolve
    EXPECT_EQ(table.resolve(h), nullptr);
}

TEST(NodeTableTest, StaleHandleReturnsNull) {
    NodeTable table;
    NodeCore node;
    
    NodeHandle h = table.alloc(&node);
    table.free(h);
    
    // Old handle with old generation
    EXPECT_EQ(table.resolve(h), nullptr);
}

TEST(NodeTableTest, NullHandle) {
    NodeTable table;
    NodeHandle null_h = NodeHandle::null_handle();
    
    EXPECT_TRUE(null_h.is_null());
    EXPECT_EQ(table.resolve(null_h), nullptr);
    EXPECT_EQ(table.state_of(null_h), NodeState::Dead);
}

TEST(NodeTableTest, MultipleAllocs) {
    NodeTable table;
    std::vector<NodeHandle> handles;
    
    for (int i = 0; i < 100; ++i) {
        NodeCore node;
        node.node_type = i % 5;
        handles.push_back(table.alloc(&node));
    }
    
    for (size_t i = 0; i < handles.size(); ++i) {
        NodeSlot* slot = table.resolve(handles[i]);
        ASSERT_NE(slot, nullptr);
        EXPECT_EQ(slot->node_type, static_cast<uint16_t>(i % 5));
    }
}

TEST(NodeTableTest, FreeListReuse) {
    NodeTable table;
    
    NodeCore node1;
    NodeHandle h1 = table.alloc(&node1);
    table.free(h1);
    
    NodeCore node2;
    NodeHandle h2 = table.alloc(&node2);
    
    EXPECT_EQ(h2.index, h1.index);
    EXPECT_EQ(h2.generation, h1.generation + 1);
}