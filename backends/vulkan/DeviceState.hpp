#pragma once

// The hidden per-device state behind RI::Vk::Device (P82), heap-allocated once
// per device so Device stays a small value, and the declarations the backend's
// sources share (formats, layouts, image barriers, internal submissions,
// validation macros).
//
// Keeps: the instance, device and volk table; the chosen memory types; the heap
// set layouts; DGC layouts; the timeline; the internal command buffer; and in
// validation builds the object and placement trackers and the device thread
// (P106). Everything is destroyed with the device.
//
// D3D12 counterpart: backends/d3d12/DeviceState.hpp.

#include <RI/Vk/Native.hpp>
#if EQUINE_VALIDATION
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/ObjectTracker.hpp>
#include <RI/Validation/Placement.hpp>

#include <thread>
#endif

#include <volk.h>

#include <array>
#include <bit>
#include <map>
#include <vector>
#include <string_view>

namespace RI::Vk {

struct DeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; // validation builds only
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32 apiVersion = 0;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable functions{};
    // VK_EXT_debug_utils (P53), instance-level functions used on this device.
    PFN_vkSetDebugUtilsObjectNameEXT setObjectName = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
    bool faultEnabled = false; // VK_EXT_device_fault (validation builds, P53 (2))
    // Indirect Level 2 (P48): VK_EXT_device_generated_commands, with RI's
    // command layouts per (kind, root data size) and a matching pipeline layout
    // per root data size (compatible with every RI pipeline of that size).
    bool generatedCommands = false;
    std::map<uint64, VkIndirectCommandsLayoutEXT> indirectLayouts;
    std::map<uint32, VkPipelineLayout> indirectPipelineLayouts;
    uint32 depthResolveModes = 0; // DeviceLimits copies (P76), for checks while recording
    uint32 stencilResolveModes = 0;
    uint32 graphicsQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;

    // Memory kinds (P32): the native memory type of each, UINT32_MAX if none.
    std::array<uint32, 4> memoryTypes{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    // Every native type that can serve each kind, best first (memoryTypes[k]
    // first): allocateMemory picks among them for resources that need it.
    std::array<std::vector<uint32>, 4> memoryTypeCandidates;
    std::array<bool, 4> memoryKindMapped{false, true, true, true};

    // Descriptor heaps (P94): one set layout per heap kind for the whole device,
    // so every pipeline layout stays compatible with every heap.
    VkDescriptorSetLayout resourceSetLayout = VK_NULL_HANDLE; // set 0
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;  // set 1
    // The mutable descriptor types of the resource heap (layout and pools
    // must agree): acceleration structures join when ray tracing is on (P49).
    std::vector<VkDescriptorType> mutableTypes;
    bool rayTracing = false;
    VkDeviceSize scratchAlignment = 256;
    // Samplers (P87): one VkSampler per distinct SamplerDesc, kept until the
    // device is destroyed; the key is the description's fields as 32-bit words.
    std::map<std::array<uint32, 13>, VkSampler> samplers;
    uint32 maxSamplers = 0;      // maxSamplerAllocationCount
    float maxAnisotropy = 1.0f;  // maxSamplerAnisotropy
    bool samplerMinMax = false;  // samplerFilterMinmax
    // The statistics counters RI asks for (P71): geometry and tessellation
    // ones only where the GPU has those stages.
    VkQueryPipelineStatisticFlags pipelineStatistics = 0; // the classic counters (QueryType::ePipelineStatistics)
    bool meshPrimitivesQuery = false;                     // meshShaderQueries: QueryType::eMeshPrimitives (P105)
    VkDeviceSize storageOffsetAlignment = 0;
    VkDeviceSize bufferImageGranularity = 1;
    VkDeviceSize uniformOffsetAlignment = 0;
    // Buffer-texture copies (P47): optimal offset and row-pitch alignments.
    VkDeviceSize copyOffsetAlignment = 1;
    VkDeviceSize copyRowPitchAlignment = 1;

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
    std::thread::id deviceThread; // the thread that called createDevice (P106)
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

// The counters of a statistics pool (P71, P105): the classic set, or the mesh
// set, which Vulkan allows during mesh draws (pixel and compute only).
inline constexpr VkQueryPipelineStatisticFlags kMeshPipelineStatistics =
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
inline VkQueryPipelineStatisticFlags statisticsOf(const DeviceState &state, QueryType type) {
    return type == QueryType::ePipelineStatistics       ? state.pipelineStatistics
           : type == QueryType::eMeshPipelineStatistics ? kMeshPipelineStatistics
                                                        : 0;
}

// Queries (Resources.cpp, P71): the bytes of one query's result. Vulkan
// writes one 64-bit value per counter of the pool, and one for every other type.
inline uint64 queryResultSize(const DeviceState &state, QueryType type) {
    const VkQueryPipelineStatisticFlags counters = statisticsOf(state, type);
    return counters != 0 ? 8ull * static_cast<uint64>(std::popcount(counters)) : 8;
}

void destroyIndirectLayouts(DeviceState &state);

// Debugging (Debug.cpp, P53).
void loadDebugFunctions(DeviceState &state);
// The native names of an object (debug utils), shared by setName and names
// given at creation (P106); `nameNative` names one handle (0: nothing).
void nameNative(const DeviceState &state, VkObjectType type, uint64 handle, std::string_view name);
void applyName(DeviceState &state, const Memory &memory, std::string_view name);
void applyName(DeviceState &state, const Buffer &buffer, std::string_view name);
void applyName(DeviceState &state, const Texture &texture, std::string_view name);
void applyName(DeviceState &state, const TextureView &view, std::string_view name);
void applyName(DeviceState &state, const DescriptorHeap &heap, std::string_view name);
void applyName(DeviceState &state, const QueryPool &pool, std::string_view name);
void applyName(DeviceState &state, const Pipeline &pipeline, std::string_view name);
void applyName(DeviceState &state, const CommandList &cmd, std::string_view name);
void applyName(DeviceState &state, const AccelerationStructure &structure, std::string_view name);
// checkVk for queue submission and waits: a lost device reports the device
// fault details when VK_EXT_device_fault is on.
void checkQueueVk(DeviceState &state, VkResult result, std::string_view call);

// Formats and layouts (Textures.cpp), shared by the backend's sources.
VkImageUsageFlags imageUsageOf(TextureUsage usage);
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
// The image view rendering uses: the 2D-array view of a cube or 3D view (P102).
inline VkImageView attachmentViewOf(const TextureView &view) {
    return reinterpret_cast<VkImageView>(view.internal.arrayView != 0 ? view.internal.arrayView : view.internal.view);
}
// The image view storage writes use: a cube's faces as a 2D array; a 3D
// texture keeps its 3D view (RWTexture3D).
inline VkImageView storageViewOf(const TextureView &view) {
    return reinterpret_cast<VkImageView>(view.internal.arrayView != 0 && !view.internal.volume ? view.internal.arrayView
                                                                                               : view.internal.view);
}
VkImageMemoryBarrier2 imageBarrier(const TextureView &view, const ImageTransition &t);
// RI's own command buffer: begin recording, then submit on the graphics queue
// (signalling the next timeline value) and wait for it.
VkCommandBuffer beginInternal(DeviceState &state);
void submitInternal(DeviceState &state);

// Validation builds: the id of a new object, and the checks on its use.
#if EQUINE_VALIDATION
#define EQ_VK_TRACK(state) ::RI::Validation::trackObject((state)->objects)
#define EQ_VK_UNTRACK(state, id) ::RI::Validation::untrackObject((state)->objects, id)
#define EQ_VK_TRACK_NAME(state, id, name) ::RI::Validation::setTrackedName((state)->objects, id, name)
#define EQ_VK_CHECK_ALIVE(state, id, kind) EQ_VALIDATE(::RI::Validation::checkAlive((state)->objects, id, kind))
// Calls outside the any-thread list run on the device thread (P106). A null
// state (a default-constructed object) is left to the liveness check.
#define EQ_VK_CHECK_THREAD(state)                                                                        \
    EQ_VALIDATE((state) == nullptr ? ::RI::Validation::CheckResult{}                                     \
                                   : ::RI::Validation::checkDeviceThread((state)->deviceThread, __func__))
#else
#define EQ_VK_TRACK(state) uint64{0}
#define EQ_VK_UNTRACK(state, id) ((void)0)
#define EQ_VK_TRACK_NAME(state, id, name) ((void)0)
#define EQ_VK_CHECK_ALIVE(state, id, kind) ((void)0)
#define EQ_VK_CHECK_THREAD(state) ((void)0)
#endif

} // namespace RI::Vk
