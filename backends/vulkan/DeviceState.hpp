#pragma once

// Backend-private state behind RI::Vk::Device (P82: hidden state behind
// `internal`). Heap-allocated once per device, so Device stays a small value.

#include <RI/Vk/Backend.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/ObjectTracker.hpp>
#endif

#include <volk.h>

namespace RI::Vk {

struct DeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; // validation builds only
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32 apiVersion = 0;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable functions{};
    uint32 graphicsQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
#endif
};

} // namespace RI::Vk
