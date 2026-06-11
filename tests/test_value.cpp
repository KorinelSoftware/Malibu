// tests/test_value.cpp
// Task 16: NaN-boxed Value round-trips; DomNodeRef NodeHandle preserved exactly.

#include <gtest/gtest.h>
#include "malibu/js/vm/value.h"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace malibu::js::vm;

TEST(Value, Int32RoundTrip) {
    for (int32_t v : {0, 1, -1, 42, -42, INT32_MAX, INT32_MIN}) {
        Value x = Value::make_int32(v);
        EXPECT_TRUE(x.is_int32());
        EXPECT_FALSE(x.is_double());
        EXPECT_EQ(x.as_int32(), v);
    }
}

TEST(Value, DoubleRoundTrip) {
    for (double v : {0.0, 1.5, -3.25, 1e9, -1e-9, 3.14159}) {
        Value x = Value::make_double(v);
        EXPECT_TRUE(x.is_double());
        EXPECT_FALSE(x.is_tagged());
        EXPECT_DOUBLE_EQ(x.as_double(), v);
    }
}

TEST(Value, NanCanonicalisedAndNotTagged) {
    Value x = Value::make_double(std::nan(""));
    EXPECT_TRUE(x.is_double());
    EXPECT_FALSE(x.is_tagged());
    EXPECT_TRUE(std::isnan(x.as_double()));
    // Infinity must not be confused with a tag either.
    Value inf = Value::make_double(std::numeric_limits<double>::infinity());
    EXPECT_TRUE(inf.is_double());
    EXPECT_FALSE(inf.is_tagged());
}

TEST(Value, BoolNullUndefined) {
    EXPECT_TRUE(Value::make_bool(true).as_bool());
    EXPECT_FALSE(Value::make_bool(false).as_bool());
    EXPECT_TRUE(Value::make_null().is_null());
    EXPECT_TRUE(Value::make_undefined().is_undefined());
    EXPECT_FALSE(Value::make_null().is_undefined());
}

TEST(Value, HeapPtrRoundTrip) {
    HeapObject obj;
    assert_heap_ptr_fits(&obj);
    Value x = Value::make_heap_ptr(&obj);
    EXPECT_TRUE(x.is_heap_ptr());
    EXPECT_EQ(x.as_heap_ptr(), &obj);
}

TEST(Value, DomNodeRefHandlePreservedNoTruncation) {
    DomNodeRef ref;
    // Use a high generation to exercise the full 32+32 bits.
    ref.handle = malibu::NodeHandle{0x12345678u, 0x9ABCDEF0u};
    Value x = Value::make_heap_ptr(&ref);
    ASSERT_TRUE(x.is_heap_ptr());
    auto* got = static_cast<DomNodeRef*>(x.as_heap_ptr());
    EXPECT_EQ(got->handle.index, 0x12345678u);
    EXPECT_EQ(got->handle.generation, 0x9ABCDEF0u);
}

TEST(Value, AsNumberCoercion) {
    EXPECT_DOUBLE_EQ(Value::make_int32(7).as_number(), 7.0);
    EXPECT_DOUBLE_EQ(Value::make_bool(true).as_number(), 1.0);
    EXPECT_DOUBLE_EQ(Value::make_null().as_number(), 0.0);
    EXPECT_TRUE(std::isnan(Value::make_undefined().as_number()));
}
