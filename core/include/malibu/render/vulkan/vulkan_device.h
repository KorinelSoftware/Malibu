#pragma once
// core/include/malibu/render/vulkan/vulkan_device.h
// Vulkan device management.

#include <cstdint>
#include <vulkan/vulkan.h>

namespace malibu::render::vulkan {

class VulkanDevice {
public:
    bool init(VkSurfaceKHR surface);
    bool recreate(VkSurfaceKHR surface);
    void destroy();
    
    [[nodiscard]] bool       valid() const { return device_ != VK_NULL_HANDLE; }
    VkInstance       instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice         device() const { return device_; }
    VkQueue          graphics_queue() const { return graphics_queue_; }
    uint32_t         graphics_family() const { return graphics_family_; }
private:
    VkInstance       instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          graphics_queue_ = VK_NULL_HANDLE;
    uint32_t         graphics_family_ = 0;
};

} // namespace malibu::render::vulkan