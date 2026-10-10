#pragma once

// Instance creation and GPU description, shared by listGpus and createDevice.
// Each device owns its own instance (P92).

#include <RI/Device.hpp>

#include <volk.h>

namespace RI::Vk {

// RI needs at least Vulkan 1.3 on the instance and the device.
inline constexpr uint32 kMinApiVersion = VK_API_VERSION_1_3;

struct InstanceHandles {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    // Fetched from this instance: volk's globals may come from another one.
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
};

enum class InstancePurpose : uint8 {
    eListing, // short-lived: no layers, no extensions
    eDevice,  // debug utils (P53), surfaces (P43), validation layer + messenger (P21)
};

InstanceHandles createInstance(InstancePurpose purpose);
void destroyInstance(InstanceHandles &handles);

// Fills `gpu` and returns true if the physical device can run RI: Vulkan 1.3
// and a queue family with graphics and compute.
bool describeGpu(VkPhysicalDevice physicalDevice, Gpu &gpu, uint32 &graphicsQueueFamily);

// Finds a listed GPU again by its id (P92). Reports a programmer error if absent.
VkPhysicalDevice findGpu(VkInstance instance, const Gpu &gpu, Gpu &found, uint32 &graphicsQueueFamily);

} // namespace RI::Vk
