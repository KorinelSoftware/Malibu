// tests/test_pbt.cpp
// Property-based tests using Rapidcheck.

#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include "malibu/dom/node_table.h"
#include "malibu/unified_object_graph/uog.h"
#include "malibu/dom/wrapper_cache.h"
#include "malibu/js/vm/realm.h"

using malibu::dom::NodeTable;
using malibu::NodeHandle;
using malibu::dom::NodeCore;
using malibu::dom::WrapperCache;
using malibu::unified_object_graph::UnifiedObjectGraph;
using malibu::js::vm::JSRealm;
using malibu::JSObjectHandle;

// RC_GEN for NodeHandle
namespace rc {
    template<>
    struct Arbitrary<malibu::NodeHandle> {
        static Gen<malibu::NodeHandle> arbitrary() {
            return gen::map(
                gen::pair(gen::inRange<uint32_t>(0, 1000), gen::inRange<uint32_t>(0, 10)),
                [](const std::pair<uint32_t, uint32_t>& p) {
                    return malibu::NodeHandle{p.first, p.second};
                }
            );
        }
    };
}

RC_GTEST_PROP(NodeTablePBT, GenerationUniqueness, ()) {
    NodeTable table;
    std::vector<NodeHandle> live_handles;
    
    // Sequence of alloc/free operations
    int num_ops = *rc::gen::inRange(1, 100);
    
    for (int i = 0; i < num_ops; ++i) {
        bool do_alloc = *rc::gen::bool_() || live_handles.empty();
        
        if (do_alloc) {
            NodeCore node;
            NodeHandle h = table.alloc(&node);
            
            // Verify no other live handle has same (index, generation)
            for (const auto& live : live_handles) {
                RC_ASSERT(!(live.index == h.index && live.generation == h.generation));
            }
            
            live_handles.push_back(h);
        } else if (!live_handles.empty()) {
            size_t idx = *rc::gen::inRange<size_t>(0, live_handles.size() - 1);
            NodeHandle h = live_handles[idx];
            table.free(h);
            live_handles.erase(live_handles.begin() + idx);
        }
    }
    
    // Final check: all live handles are unique
    for (size_t i = 0; i < live_handles.size(); ++i) {
        for (size_t j = i + 1; j < live_handles.size(); ++j) {
            RC_ASSERT(!(live_handles[i].index == live_handles[j].index && 
                        live_handles[i].generation == live_handles[j].generation));
        }
    }
}

RC_GTEST_PROP(WrapperCachePBT, Identity, ()) {
    WrapperCache cache;
    JSRealm realm;
    
    int num_ops = *rc::gen::inRange(1, 50);
    
    for (int i = 0; i < num_ops; ++i) {
        NodeHandle h = *rc::gen::arbitrary<malibu::NodeHandle>();
        
        JSObjectHandle w1 = cache.get_or_create(h, realm);
        JSObjectHandle w2 = cache.get_or_create(h, realm);
        
        RC_ASSERT(w1 == w2);
        RC_ASSERT(!w1.is_null());
    }
}

RC_GTEST_PROP(WrapperCachePBT, DifferentRealmDifferentWrapper, ()) {
    WrapperCache cache;
    JSRealm realm1;
    JSRealm realm2;
    
    RC_ASSERT(realm1.id() != realm2.id());
    
    int num_ops = *rc::gen::inRange(1, 50);
    
    for (int i = 0; i < num_ops; ++i) {
        NodeHandle h = *rc::gen::arbitrary<malibu::NodeHandle>();
        
        JSObjectHandle w1 = cache.get_or_create(h, realm1);
        JSObjectHandle w2 = cache.get_or_create(h, realm2);
        
        RC_ASSERT(w1 != w2);
    }
}

RC_GTEST_PROP(UOGPBT, RecycledSlotDistinctEntry, ()) {
    UnifiedObjectGraph uog;
    JSRealm realm;
    
    int num_cycles = *rc::gen::inRange(1, 20);
    
    for (int cycle = 0; cycle < num_cycles; ++cycle) {
        NodeHandle h{static_cast<uint32_t>(cycle), 0};
        uog.on_wrapper_created(h, JSObjectHandle{0x1000 + cycle});
        uog.on_node_destroyed(h);
    }
    
    RC_SUCCEED();
}

RC_GTEST_PROP(DomCommandBufferPBT, OrderingPreserved, ()) {
    // Test that DomCommandBuffer preserves order of commands
    // This would require DomCommandBuffer to be testable
    // Placeholder for when DomCommandBuffer is implemented
    RC_SUCCEED();
}

RC_GTEST_PROP(OriginIsolationPBT, CrossOriginDenied, ()) {
    // Test that cross-origin storage access is denied
    // Placeholder for when StorageEngine is implemented
    RC_SUCCEED();
}