// tests/test_vulkan.cpp
// Task 25: Vulkan device bring-up. The Vulkan loader/ICD leaks via D-Bus on
// instance creation (third-party, unsymbolized), so this binary runs with leak
// detection disabled (see CMake ENVIRONMENT). The device code itself is RAII.

#include <gtest/gtest.h>
#include "malibu/render/vulkan/vulkan_device.h"

using malibu::render::vulkan::VulkanDevice;

TEST(Vulkan, DeviceInitAndDestroy) {
    // Offscreen init (null surface): returns true when a GPU/ICD is present,
    // false gracefully otherwise. Must never crash; destroy() is idempotent.
    VulkanDevice dev;
    bool ok = dev.init(VK_NULL_HANDLE);
    EXPECT_EQ(ok, dev.valid());
    if (ok) {
        EXPECT_NE(dev.device(), VK_NULL_HANDLE);
        EXPECT_NE(dev.graphics_queue(), VK_NULL_HANDLE);
    }
    dev.destroy();
    EXPECT_FALSE(dev.valid());
    dev.destroy();  // double destroy is safe
}

TEST(Vulkan, RecreateReinitialises) {
    VulkanDevice dev;
    if (dev.init(VK_NULL_HANDLE)) {
        EXPECT_TRUE(dev.recreate(VK_NULL_HANDLE));
        EXPECT_TRUE(dev.valid());
        dev.destroy();
    } else {
        SUCCEED();  // no GPU available in this environment
    }
}
