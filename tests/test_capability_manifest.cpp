// tests/test_capability_manifest.cpp
// Task 4: every JSON key queryable; unknown key returns false.

#include "malibu/capability_manifest/capability_manifest.h"
#include <gtest/gtest.h>

using malibu::capability_manifest::CapabilityManifest;

TEST(CapabilityManifest, ParsesEmbeddedManifest) {
    auto& m = CapabilityManifest::instance();
    EXPECT_GT(m.feature_count(), 0u);
}

TEST(CapabilityManifest, SupportedFeaturesReturnTrue) {
    auto& m = CapabilityManifest::instance();
    EXPECT_TRUE(m.is_supported("querySelector"));
    EXPECT_TRUE(m.is_supported("addEventListener"));
    EXPECT_TRUE(m.is_supported("WebCallABI"));
    EXPECT_TRUE(m.is_supported("UnifiedObjectGraph"));
}

TEST(CapabilityManifest, UnsupportedFeaturesReturnFalseButAreKnown) {
    auto& m = CapabilityManifest::instance();
    EXPECT_FALSE(m.is_supported("WebGPU"));
    EXPECT_FALSE(m.is_supported("fetch"));
    EXPECT_TRUE(m.is_known("WebGPU"));
    EXPECT_TRUE(m.is_known("fetch"));
}

TEST(CapabilityManifest, UnknownKeyReturnsFalseAndIsNotKnown) {
    auto& m = CapabilityManifest::instance();
    EXPECT_FALSE(m.is_supported("ThisApiDoesNotExist"));
    EXPECT_FALSE(m.is_known("ThisApiDoesNotExist"));
}

TEST(CapabilityManifest, MetadataCommentNotTreatedAsFeature) {
    auto& m = CapabilityManifest::instance();
    // "_comment" is a string, not a bool, so it must be skipped entirely.
    EXPECT_FALSE(m.is_known("_comment"));
}
