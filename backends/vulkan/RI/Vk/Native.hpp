#pragma once

// Native Vulkan access (P84). Including this header and naming the backend
// (`RI::Vk::native(device)`) is the only way to reach Vulkan objects.

#include <RI/Vk/Backend.hpp>

#include <volk.h>

#include <span>

namespace RI::Vk {

struct NativeDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // Device-level functions for this device (P89). Core instance-level functions are volk's
    // globals; fetch instance extension functions with vkGetInstanceProcAddr(instance, ...).
    const VolkDeviceTable *functions = nullptr;
    uint32 graphicsQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32 apiVersion = 0; // the device's Vulkan version, at least 1.3
};

NativeDevice native(const Device &device);

// The features RI enables (P60 (2), P92): every feature the GPU reports,
// minus robustness and capture/replay. Get them with defaultDeviceFeatures,
// change any field (e.g. turn robustness on), and pass them to createDevice.
// RI links the structures itself, so sType and pNext are ignored. Members of
// extensions RI does not enable on this GPU are ignored; v14 is used only on
// Vulkan 1.4 devices. After your changes RI still turns off features whose
// prerequisites are off (e.g. update-after-bind buffers when robust buffer
// access cannot support them), so the device stays valid.
struct DeviceFeatures {
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    VkPhysicalDeviceVulkan14Features v14{};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
#ifdef VK_KHR_unified_image_layouts
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedLayouts{};
#endif
    VkPhysicalDeviceFaultFeaturesEXT fault{};
};

DeviceFeatures defaultDeviceFeatures(const Gpu &gpu);

// Native additions at device creation (P60 (3)).
struct NativeDeviceAdditions {
    // Extra device extensions, enabled alongside RI's own.
    std::span<const char *const> extensions;
    // RI's features as changed by you (start from defaultDeviceFeatures).
    // nullptr: RI's default.
    const DeviceFeatures *features = nullptr;
    // Extra structures for VkDeviceCreateInfo, appended after RI's chain and
    // left untouched. Not for the structures in DeviceFeatures or their
    // single-feature equivalents (e.g. VkPhysicalDeviceImageRobustnessFeatures):
    // Vulkan forbids chaining both; change DeviceFeatures instead.
    const void *pNext = nullptr;
};

Device createDevice(const DeviceDesc &desc, const NativeDeviceAdditions &additions);

} // namespace RI::Vk
