#pragma once

// Backend-private state behind RI::Vk::Device (P82: hidden state behind
// `internal`). Heap-allocated once per device, so Device stays a small value.

#include <RI/Vk/Backend.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/ObjectTracker.hpp>
#endif

#include <volk.h>

#include <array>

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

    // Memory kinds (P32): the native memory type of each, UINT32_MAX if none.
    std::array<uint32, 4> memoryTypes{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::array<bool, 4> memoryKindMapped{false, true, true, true};

    // Descriptor heaps (P94): one set layout per heap kind for the whole device,
    // so every pipeline layout stays compatible with every heap.
    VkDescriptorSetLayout resourceSetLayout = VK_NULL_HANDLE; // set 0
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;  // set 1
    VkDeviceSize storageOffsetAlignment = 0;
    VkDeviceSize uniformOffsetAlignment = 0;

    // The graphics queue's timeline (P20).
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64 lastSignaled = 0;

#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
#endif
};

inline uint32 kindIndex(MemoryKind kind) { return static_cast<uint32>(kind); }

// Validation builds: the id of a new object, and the checks on its use.
#if EQUINE_VALIDATION
#define EQ_VK_TRACK(state) ::RI::Validation::trackObject((state)->objects)
#define EQ_VK_UNTRACK(state, id) ::RI::Validation::untrackObject((state)->objects, id)
#define EQ_VK_CHECK_ALIVE(state, id, kind) EQ_VALIDATE(::RI::Validation::checkAlive((state)->objects, id, kind))
#else
#define EQ_VK_TRACK(state) uint64{0}
#define EQ_VK_UNTRACK(state, id) ((void)0)
#define EQ_VK_CHECK_ALIVE(state, id, kind) ((void)0)
#endif

} // namespace RI::Vk
