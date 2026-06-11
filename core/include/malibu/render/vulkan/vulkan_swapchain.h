#pragma once
// core/include/malibu/render/vulkan/vulkan_swapchain.h
// Vulkan swapchain management.

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include "vulkan_device.h"

namespace malibu::render::vulkan {

class VulkanSwapchain {
public:
    bool init(VulkanDevice& dev, VkSurfaceKHR surface, VkExtent2D extent);
    bool resize(VulkanDevice& dev, VkSurfaceKHR surface, VkExtent2D extent);
    void destroy();
    
    VkSwapchainKHR handle() const { return swapchain_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    const std::vector<VkImage>& images() const { return images_; }
    const std::vector<VkImageView>& image_views() const { return image_views_; }
private:
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    VkExtent2D extent_;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_SRGB;
};

} // namespace malibu::render::vulkan