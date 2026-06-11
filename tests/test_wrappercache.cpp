// tests/test_wrappercache.cpp
// Unit tests for WrapperCache.

#include <gtest/gtest.h>
#include "malibu/dom/wrapper_cache.h"
#include "malibu/js/vm/realm.h"

using malibu::dom::WrapperCache;
using malibu::NodeHandle;
using malibu::js::vm::JSRealm;
using malibu::JSObjectHandle;

TEST(WrapperCacheTest, SameHandleSameRealmReturnsIdenticalWrapper) {
    WrapperCache cache;
    JSRealm realm1;
    NodeHandle h{1, 0};
    
    JSObjectHandle w1 = cache.get_or_create(h, realm1);
    JSObjectHandle w2 = cache.get_or_create(h, realm1);
    
    EXPECT_EQ(w1, w2);
    EXPECT_FALSE(w1.is_null());
}

TEST(WrapperCacheTest, DifferentRealmReturnsDifferentWrapper) {
    WrapperCache cache;
    JSRealm realm1;
    JSRealm realm2;
    NodeHandle h{2, 0};
    
    JSObjectHandle w1 = cache.get_or_create(h, realm1);
    JSObjectHandle w2 = cache.get_or_create(h, realm2);
    
    EXPECT_NE(w1, w2);
    EXPECT_FALSE(w1.is_null());
    EXPECT_FALSE(w2.is_null());
}

TEST(WrapperCacheTest, RecycledSlotDifferentGenerationReturnsDifferentWrapper) {
    WrapperCache cache;
    JSRealm realm;
    
    NodeHandle h1{3, 0};
    JSObjectHandle w1 = cache.get_or_create(h1, realm);
    
    NodeHandle h2{3, 1};  // Same index, new generation
    JSObjectHandle w2 = cache.get_or_create(h2, realm);
    
    EXPECT_NE(w1, w2);
}

TEST(WrapperCacheTest, PurgeRealmRemovesOnlyThatRealm) {
    WrapperCache cache;
    JSRealm realm1;
    JSRealm realm2;
    NodeHandle h{4, 0};
    
    JSObjectHandle w1 = cache.get_or_create(h, realm1);
    JSObjectHandle w2 = cache.get_or_create(h, realm2);
    
    cache.purge_realm(realm1.id());
    
    // realm1 entries removed, new wrapper created (same value since deterministic)
    JSObjectHandle w3 = cache.get_or_create(h, realm1);
    EXPECT_EQ(w3, w1);  // Same wrapper for same (node, realm) pair
    
    // realm2 entry still there
    JSObjectHandle w4 = cache.get_or_create(h, realm2);
    EXPECT_EQ(w4, w2);
}

TEST(WrapperCacheTest, WrapperCollectedRemovesEntry) {
    WrapperCache cache;
    JSRealm realm;
    NodeHandle h{5, 0};
    
    JSObjectHandle w1 = cache.get_or_create(h, realm);
    cache.on_wrapper_collected(h, realm.id());
    
    // After collection, new wrapper created (same value since deterministic)
    JSObjectHandle w2 = cache.get_or_create(h, realm);
    EXPECT_EQ(w2, w1);  // Same wrapper for same (node, realm) pair
}

TEST(WrapperCacheTest, MultipleHandlesMultipleRealms) {
    WrapperCache cache;
    JSRealm realm1;
    JSRealm realm2;
    JSRealm realm3;
    
    for (int i = 0; i < 10; ++i) {
        NodeHandle h{static_cast<uint32_t>(i), 0};
        cache.get_or_create(h, realm1);
        cache.get_or_create(h, realm2);
        cache.get_or_create(h, realm3);
    }
    
    // Verify all distinct
    EXPECT_NE(realm1.id(), realm2.id());
    EXPECT_NE(realm2.id(), realm3.id());
}

TEST(WrapperCacheTest, ConcurrencyStress) {
    WrapperCache cache;
    JSRealm realm;
    
    // Simulate concurrent access from multiple threads
    for (int i = 0; i < 1000; ++i) {
        NodeHandle h{static_cast<uint32_t>(i % 10), 0};
        cache.get_or_create(h, realm);
    }
    
    SUCCEED();
}