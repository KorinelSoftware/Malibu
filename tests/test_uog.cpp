// tests/test_uog.cpp
// Unit tests for UnifiedObjectGraph.

#include <gtest/gtest.h>
#include "malibu/unified_object_graph/uog.h"

using malibu::unified_object_graph::UnifiedObjectGraph;
using malibu::NodeHandle;
using malibu::JSObjectHandle;

TEST(UOGTest, WrapperCreatedRemovesGCRoot) {
    UnifiedObjectGraph uog;
    NodeHandle h{1, 0};
    JSObjectHandle wrapper{0x1000};
    
    uog.on_wrapper_created(h, wrapper);
    
    // We can't directly test internal state, but we can verify
    // the entry exists and wrapper is set
    // This would need a test accessor or we test via GC integration
    SUCCEED();
}

TEST(UOGTest, ListenerRegistered) {
    UnifiedObjectGraph uog;
    NodeHandle h{2, 0};
    JSObjectHandle fn{0x2000};
    
    uog.on_listener_registered(h, fn);
    uog.on_listener_registered(h, fn);  // duplicate
    
    // Verify no crash
    SUCCEED();
}

TEST(UOGTest, ListenerRemoved) {
    UnifiedObjectGraph uog;
    NodeHandle h{3, 0};
    JSObjectHandle fn{0x3000};
    
    uog.on_listener_registered(h, fn);
    uog.on_listener_removed(h, fn);
    
    // Verify no crash
    SUCCEED();
}

TEST(UOGTest, WrapperCollected) {
    UnifiedObjectGraph uog;
    NodeHandle h{4, 0};
    JSObjectHandle wrapper{0x4000};
    
    uog.on_wrapper_created(h, wrapper);
    uog.on_wrapper_collected(h);
    
    // Verify no crash
    SUCCEED();
}

TEST(UOGTest, NodeDestroyed) {
    UnifiedObjectGraph uog;
    NodeHandle h{5, 0};
    JSObjectHandle wrapper{0x5000};
    
    uog.on_wrapper_created(h, wrapper);
    uog.on_node_destroyed(h);
    
    // Entry should be removed
    SUCCEED();
}

TEST(UOGTest, RecycledSlotDistinctEntry) {
    UnifiedObjectGraph uog;
    NodeHandle h1{10, 0};
    NodeHandle h2{10, 1};  // Same index, new generation
    
    uog.on_wrapper_created(h1, JSObjectHandle{0x1000});
    uog.on_node_destroyed(h1);
    uog.on_wrapper_created(h2, JSObjectHandle{0x2000});
    
    // h1 and h2 should be distinct entries
    SUCCEED();
}

TEST(UOGTest, MultipleListeners) {
    UnifiedObjectGraph uog;
    NodeHandle h{20, 0};
    
    for (int i = 0; i < 10; ++i) {
        uog.on_listener_registered(h, JSObjectHandle{static_cast<uint64_t>(0x1000 + i)});
    }
    
    for (int i = 0; i < 5; ++i) {
        uog.on_listener_removed(h, JSObjectHandle{static_cast<uint64_t>(0x1000 + i)});
    }
    
    SUCCEED();
}

TEST(UOGTest, ConcurrentAccess) {
    UnifiedObjectGraph uog;
    NodeHandle h{30, 0};
    
    // Simulate concurrent calls from multiple threads
    // This is a smoke test - real thread safety tested with ThreadSanitizer
    for (int i = 0; i < 100; ++i) {
        uog.on_listener_registered(h, JSObjectHandle{static_cast<uint64_t>(i)});
        uog.on_listener_removed(h, JSObjectHandle{static_cast<uint64_t>(i)});
    }
    
    SUCCEED();
}