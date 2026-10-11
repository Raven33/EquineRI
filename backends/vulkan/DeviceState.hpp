#pragma once

// Backend-private state behind RI::Vk::Device (P82: hidden state behind
// `internal`). Heap-allocated once per device, so Device stays a small value.

#include <RI/Vk/Native.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/ObjectTracker.hpp>
#include <RI/Validation/Placement.hpp>
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
    VkDeviceSize bufferImageGranularity = 1;
    VkDeviceSize uniformOffsetAlignment = 0;

    // The graphics queue's timeline (P20).
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64 lastSignaled = 0;

    // How image layouts are hidden (L1, P78, P95); never eAuto.
    LayoutMode layoutMode = LayoutMode::eTransitions;
    bool hostBarrierAtEnd = true; // P94 (6); off only to measure it

    // RI's own command buffer, for the initial reset of new textures (R16, P69).
    VkCommandPool internalPool = VK_NULL_HANDLE;
    VkCommandBuffer internalCommands = VK_NULL_HANDLE;

#if EQUINE_VALIDATION
    Validation::ObjectTracker objects;
    Validation::PlacementTracker placements; // discard after aliasing (P69)
#endif
};

inline uint32 kindIndex(MemoryKind kind) { return static_cast<uint32>(kind); }

// Buffers and textures may be neighbours in one memory block (P31): starting
// and ending each on bufferImageGranularity keeps them off each other's pages.
inline MemoryRequirements withGranularity(const DeviceState &state, MemoryRequirements requirements) {
    const uint64 granularity = state.bufferImageGranularity;
    requirements.alignment = requirements.alignment > granularity ? requirements.alignment : granularity;
    requirements.size = (requirements.size + granularity - 1) / granularity * granularity;
    return requirements;
}
// Every memory kind whose native type is in `typeBits`, as MemoryRequirements::kinds.
uint32 kindsOf(const DeviceState &state, uint32 typeBits);

// Formats and layouts (Textures.cpp), shared by the backend's sources.
VkFormat toVkFormat(Format format);
const char *formatName(Format format);
// The layout `usage` rests in under `mode` (L1, P95).
VkImageLayout restingLayoutOf(LayoutMode mode, Format format, TextureUsage usage);

// Image barriers (Commands.cpp and Textures.cpp).
struct ImageTransition {
    VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 dstAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};
VkImageSubresourceRange rangeOf(const TextureView &view);
VkImageMemoryBarrier2 imageBarrier(const TextureView &view, const ImageTransition &t);
// RI's own command buffer: begin recording, then submit on the graphics queue
// (signalling the next timeline value) and wait for it.
VkCommandBuffer beginInternal(DeviceState &state);
void submitInternal(DeviceState &state);

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
