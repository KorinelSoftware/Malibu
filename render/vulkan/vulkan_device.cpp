// render/vulkan/vulkan_device.cpp
// Real Vulkan device bring-up: instance, physical-device selection, logical
// device + graphics queue. Fails gracefully (returns false) when no Vulkan
// driver / GPU is available, so headless environments do not crash.

#include "malibu/render/vulkan/vulkan_device.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <vector>

namespace malibu::render::vulkan {

bool VulkanDevice::init(VkSurfaceKHR surface) {
    // --- Instance ---
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Malibu";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        MALIBU_LOG(WARNING, "vulkan", "vkCreateInstance failed (no Vulkan driver?)");
        instance_ = VK_NULL_HANDLE;
        return false;
    }

    // --- Physical device ---
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        MALIBU_LOG(WARNING, "vulkan", "no Vulkan physical devices available");
        destroy();
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t gfx_family = 0;
    for (VkPhysicalDevice pd : devices) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> props(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, props.data());
        for (uint32_t i = 0; i < qcount; ++i) {
            bool graphics = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            bool present = true;
            if (surface != VK_NULL_HANDLE) {
                VkBool32 sup = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &sup);
                present = (sup == VK_TRUE);
            }
            if (graphics && present) { chosen = pd; gfx_family = i; break; }
        }
        if (chosen != VK_NULL_HANDLE) break;
    }
    if (chosen == VK_NULL_HANDLE) {
        MALIBU_LOG(WARNING, "vulkan", "no suitable graphics queue family");
        destroy();
        return false;
    }
    physical_ = chosen;
    graphics_family_ = gfx_family;

    // --- Logical device + queue ---
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfx_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    if (vkCreateDevice(chosen, &dci, nullptr, &device_) != VK_SUCCESS) {
        MALIBU_LOG(ERROR, "vulkan", "vkCreateDevice failed");
        destroy();
        return false;
    }
    vkGetDeviceQueue(device_, gfx_family, 0, &graphics_queue_);
    MALIBU_LOG(INFO, "vulkan", "Vulkan device initialised");
    return true;
}

bool VulkanDevice::recreate(VkSurfaceKHR surface) {
    destroy();
    return init(surface);
}

void VulkanDevice::destroy() {
    if (device_ != VK_NULL_HANDLE) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (instance_ != VK_NULL_HANDLE) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    graphics_queue_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    graphics_family_ = 0;
}

} // namespace malibu::render::vulkan
