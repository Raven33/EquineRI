// Command lists and recording on Vulkan (P20, P34, P36, P56, P85).
//
// Owns: command lists (one VkCommandPool each), heap, pipeline and root data
// binding, barriers, rendering sections (with attachments, or a size alone,
// P105) and draws, discard and clears,
// queries, buffer-texture copies, resolve, native sections, submit and wait.
//
// Keeps: what is bound, in CommandList::internal (heap sets, pipeline layout,
// root data size, the open rendering section), so RI can rebind after a
// pipeline-layout change and restore state after a native section (P70).
//
// Watch out: textures change layout only here and in Textures.cpp, through
// imageBarrier around RI's own operations (A', P68). The viewport has a negative
// height so clip space is y-up as on D3D12. cmdEnd adds a barrier to the host
// (P94 (6)). A barrier on Stage::eIndirect also covers DGC preprocess (P48).
//
// D3D12 counterpart: backends/d3d12/Commands.cpp and Rendering.cpp.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>
#include <RI/Vk/Native.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <vector>

namespace RI::Vk {

namespace {

const VolkDeviceTable &fn(const CommandList &cmd) { return cmd.internal.device->functions; }

VkPipelineStageFlags2 stagesOf(Stage stage) {
    if ((stage & Stage::eAll) != Stage::eNone) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkPipelineStageFlags2 flags = VK_PIPELINE_STAGE_2_NONE;
    if ((stage & Stage::eCopy) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    }
    if ((stage & Stage::eCompute) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if ((stage & Stage::eIndirect) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if ((stage & Stage::eVertex) != Stage::eNone) {
        // Every stage before rasterization, as D3D12's VERTEX_SHADING (P102).
        flags |= VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    }
    if ((stage & Stage::ePixel) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if ((stage & Stage::eRenderTarget) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if ((stage & Stage::eDepthStencil) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if ((stage & Stage::eAccelerationStructure) != Stage::eNone) {
        flags |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    }
    return flags;
}

uint32 bindIndex(bool graphics) { return graphics ? 1 : 0; }
VkPipelineBindPoint bindPointOf(uint32 index) {
    return index == 1 ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
}

// Binds the heap's sets at one bind point (0 compute, 1 graphics) with that
// bind point's pipeline layout. Pipelines with different root data sizes have
// incompatible layouts, so the sets are bound again whenever the layout or the
// heap changes (P70: RI's implicit state).
void bindHeap(CommandList &cmd, uint32 index) {
    if (cmd.internal.boundLayout[index] == 0 || cmd.internal.heapResourceSet == 0) {
        return;
    }
    const VkDescriptorSet sets[] = {reinterpret_cast<VkDescriptorSet>(cmd.internal.heapResourceSet),
                                    reinterpret_cast<VkDescriptorSet>(cmd.internal.heapSamplerSet)};
    fn(cmd).vkCmdBindDescriptorSets(native(cmd), bindPointOf(index),
                                    reinterpret_cast<VkPipelineLayout>(cmd.internal.boundLayout[index]), 0, 2, sets,
                                    0, nullptr);
}

void pipelineBarrier(const CommandList &cmd, std::span<const VkImageMemoryBarrier2> barriers) {
    if (barriers.empty()) {
        return;
    }
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = static_cast<uint32>(barriers.size()),
                                      .pImageMemoryBarriers = barriers.data()};
    fn(cmd).vkCmdPipelineBarrier2(native(cmd), &dependency);
}


VkAttachmentLoadOp loadOf(LoadOp op) {
    switch (op) {
    case LoadOp::eLoad: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::eClear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::eDontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

// Read-only aspects store nothing (P75): a store would be a write.
VkAttachmentStoreOp storeOf(StoreOp op, bool readOnly) {
    if (readOnly) {
        return VK_ATTACHMENT_STORE_OP_NONE;
    }
    return op == StoreOp::eStore ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// The layout of a depth attachment during a rendering section (P75). GENERAL
// in unified and G modes. Mode T uses sync2's generic layouts where they apply,
// so a texture with eReadOnlyDepthSampling (resting in READ_ONLY_OPTIMAL) needs
// no switch in a read-only section; other sampled depth (resting in
// SHADER_READ_ONLY_OPTIMAL, P97) switches in and out.
VkImageLayout depthSectionLayout(LayoutMode mode, bool depthReadOnly, bool stencilReadOnly, bool stencil) {
    if (mode != LayoutMode::eTransitions) {
        return VK_IMAGE_LAYOUT_GENERAL;
    }
    const bool stencilRO = stencil ? stencilReadOnly : depthReadOnly;
    if (depthReadOnly && stencilRO) {
        return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    }
    if (!depthReadOnly && !stencilRO) {
        return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    }
    return depthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
                         : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
}

constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kAllAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

// Resolves (P52, P76): they write the target in the attachment stages.
constexpr VkPipelineStageFlags2 kResolveStages =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kResolveAccess =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

VkResolveModeFlagBits resolveModeOf(ResolveMode mode) {
    switch (mode) {
    case ResolveMode::eAverage: return VK_RESOLVE_MODE_AVERAGE_BIT;
    case ResolveMode::eMin: return VK_RESOLVE_MODE_MIN_BIT;
    case ResolveMode::eMax: return VK_RESOLVE_MODE_MAX_BIT;
    case ResolveMode::eSampleZero: return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }
    return VK_RESOLVE_MODE_AVERAGE_BIT;
}

[[maybe_unused]] Validation::ResolveCheck resolveCheckOf(const DeviceState &state, const TextureView &src,
                                                         const TextureView &dst, ResolveMode mode) {
    const TextureUsage attachment = isDepthFormat(dst.format) ? TextureUsage::eDepthStencil : TextureUsage::eRenderTarget;
    return {.srcSamples = src.samples,
            .dstSamples = dst.samples,
            .srcWidth = src.width,
            .srcHeight = src.height,
            .srcLayers = src.internal.layerCount,
            .dstWidth = dst.width,
            .dstHeight = dst.height,
            .dstLayers = dst.internal.layerCount,
            .srcFormat = src.format,
            .dstFormat = dst.format,
            .dstHasAttachmentUsage = (dst.internal.textureUsage & attachment) != TextureUsage::eNone,
            .mode = mode,
            .depthModes = state.depthResolveModes,
            .stencilModes = state.stencilResolveModes};
}

// The switch of a resolve target into `layout` before the section: its old
// contents are replaced.
VkImageMemoryBarrier2 resolveTargetIn(const TextureView &view, VkImageLayout layout) {
    return imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccess = VK_ACCESS_2_NONE,
                               .dstStage = kResolveStages,
                               .dstAccess = kResolveAccess,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = layout});
}

uint64 completedValue(DeviceState &state) {
    uint64 value = 0;
    checkQueueVk(state, state.functions.vkGetSemaphoreCounterValue(state.device, state.timeline, &value),
                 "vkGetSemaphoreCounterValue");
    return value;
}

} // namespace

// ── Command lists (P56) ──────────────────────────────────────────────────────

CommandList createCommandList(Device &device, std::string_view name) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .queueFamilyIndex = state.graphicsQueueFamily};
    VkCommandPool pool = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateCommandPool(state.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(state.functions.vkAllocateCommandBuffers(state.device, &allocInfo, &commandBuffer),
            "vkAllocateCommandBuffers");

    CommandList cmd;
    cmd.internal.device = &state;
    cmd.internal.commandBuffer = commandBuffer;
    cmd.internal.pool = reinterpret_cast<uint64>(pool);
    cmd.internal.id = EQ_VK_TRACK(&state);
    if (!name.empty()) {
        applyName(*device.internal.state, cmd, name);
    }
    return cmd;
}

void destroyCommandList(Device &device, CommandList &cmd) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "destroying"));
    EQ_VK_UNTRACK(&state, cmd.internal.id);
    state.functions.vkDestroyCommandPool(state.device, reinterpret_cast<VkCommandPool>(cmd.internal.pool), nullptr);
    cmd = {};
}

void resetCommandList(CommandList &cmd) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "resetting"));
    checkVk(state.functions.vkResetCommandPool(state.device, reinterpret_cast<VkCommandPool>(cmd.internal.pool), 0),
            "vkResetCommandPool");
}

// ── Recording (P34: each call writes the native command at once) ─────────────

void cmdBegin(CommandList &cmd) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, cmd.internal.id, "command list");
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    checkVk(fn(cmd).vkBeginCommandBuffer(native(cmd), &info), "vkBeginCommandBuffer");
    cmd.internal.boundLayout[0] = 0;
    cmd.internal.boundLayout[1] = 0;
    cmd.internal.graphics = false;
    cmd.internal.rendering = false;
    cmd.internal.markerDepth = 0;
    cmd.internal.openStatistics = 0;
    cmd.internal.pipeline = 0;
    cmd.internal.boundPipeline[0] = 0;
    cmd.internal.boundPipeline[1] = 0;
    cmd.internal.rootDataBytes = 0;
    cmd.internal.native = false;
    cmd.internal.depthReadOnly = false;
    cmd.internal.heapResourceSet = 0;
    cmd.internal.heapSamplerSet = 0;
    cmd.internal.rootDataSize = 0;
}

void cmdEnd(CommandList &cmd) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdEnd"));
    EQ_VALIDATE(Validation::checkMarkersClosed(cmd.internal.markerDepth));
    // Writes become visible to the CPU once the submission's timeline point is
    // reached, as on D3D12 (Vulkan needs an explicit host barrier for this).
    const VkMemoryBarrier2 toHost{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &toHost};
    if (cmd.internal.device->hostBarrierAtEnd) {
        fn(cmd).vkCmdPipelineBarrier2(native(cmd), &dependency);
    }
    checkVk(fn(cmd).vkEndCommandBuffer(native(cmd)), "vkEndCommandBuffer");
}

void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, heap.internal.id, "descriptor heap");
    cmd.internal.heapResourceSet = heap.internal.resourceSet;
    cmd.internal.heapSamplerSet = heap.internal.samplerSet;
    bindHeap(cmd, 0);
    bindHeap(cmd, 1);
}

void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pipeline.internal.id, "pipeline");
    EQ_VALIDATE(Validation::checkHeapSetBeforePipeline(cmd.internal.heapResourceSet != 0));
    EQ_VALIDATE(Validation::checkDepthWriteAllowed(cmd.internal.rendering && cmd.internal.depthReadOnly,
                                                   pipeline.internal.depthWrite));
    const uint32 index = bindIndex(pipeline.internal.graphics);
    fn(cmd).vkCmdBindPipeline(native(cmd), bindPointOf(index), native(pipeline).pipeline);
    cmd.internal.graphics = pipeline.internal.graphics;
    cmd.internal.rootDataSize = pipeline.rootDataSize;
    cmd.internal.pipeline = pipeline.internal.pipeline;
    cmd.internal.boundPipeline[bindIndex(pipeline.internal.graphics)] = pipeline.internal.pipeline;
    if (cmd.internal.boundLayout[index] != pipeline.internal.layout) {
        cmd.internal.boundLayout[index] = pipeline.internal.layout;
        bindHeap(cmd, index);
    }
    // The pipeline's reference, unless cmdSetStencilReference set one in this section (P104).
    if (pipeline.internal.graphics) {
        if (!cmd.internal.stencilOverride) {
            cmd.internal.stencilReference = pipeline.internal.stencilReference;
        }
        fn(cmd).vkCmdSetStencilReference(native(cmd), VK_STENCIL_FACE_FRONT_AND_BACK, cmd.internal.stencilReference);
    }
}

void cmdSetRootData(CommandList &cmd, std::span<const std::byte> data) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRootDataSize(data.size(), cmd.internal.rootDataSize));
    const auto size = static_cast<uint32>(data.size());
    // Kept for endNative (P70).
    std::memcpy(cmd.internal.rootData, data.data(), std::min<size_t>(size, sizeof(cmd.internal.rootData)));
    cmd.internal.rootDataBytes = size;
    fn(cmd).vkCmdPushConstants(native(cmd),
                               reinterpret_cast<VkPipelineLayout>(cmd.internal.boundLayout[bindIndex(cmd.internal.graphics)]),
                               VK_SHADER_STAGE_ALL, 0, size, data.data());
}

void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkGroupCounts(x, y, z, false, "cmdDispatch"));
    fn(cmd).vkCmdDispatch(native(cmd), x, y, z);
}

// Coarse stage -> stage barrier (P36): every write before, every access after.
void cmdBarrier(CommandList &cmd, Stage before, Stage after) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    if (((before | after) & Stage::eAccelerationStructure) != Stage::eNone && !cmd.internal.device->rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    "cmdBarrier: Stage::eAccelerationStructure needs ray tracing (DeviceLimits::rayTracing)");
    }
    // Indirect Level 2 reads and writes its scratch in the preprocess stage (P48).
    const auto stages = [&](Stage stage) {
        VkPipelineStageFlags2 flags = stagesOf(stage);
        if ((stage & Stage::eIndirect) != Stage::eNone && cmd.internal.device->generatedCommands) {
            flags |= VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT;
        }
        return flags;
    };
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = stages(before),
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = stages(after),
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    fn(cmd).vkCmdPipelineBarrier2(native(cmd), &dependency);
}

void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, src.internal.id, "buffer");
    EQ_VK_CHECK_ALIVE(cmd.internal.device, dst.internal.id, "buffer");
    const VkBufferCopy region{.srcOffset = srcOffset, .dstOffset = dstOffset, .size = size};
    fn(cmd).vkCmdCopyBuffer(native(cmd), native(src), native(dst), 1, &region);
}

// ── Rendering sections (P40, P68, P75) ──────────────────────────────────────

namespace {

bool isUintFormat(Format format) {
    return format == Format::eR32Uint || format == Format::eRG32Uint || format == Format::eRGBA32Uint;
}

VkClearColorValue clearColorOf(Format format, const ClearValue &value) {
    VkClearColorValue color{};
    for (size_t i = 0; i < 4; ++i) {
        if (isUintFormat(format)) {
            color.uint32[i] = static_cast<uint32>(value.color[i]);
        } else {
            color.float32[i] = value.color[i];
        }
    }
    return color;
}

// D3D12's orientation: a negative height makes clip-space y point up on both
// backends, so the rectangle starts at its bottom edge.
void applyViewport(CommandList &cmd) {
    const Viewport &v = cmd.internal.viewport;
    const VkViewport viewport{.x = v.x,
                              .y = v.y + v.height,
                              .width = v.width,
                              .height = -v.height,
                              .minDepth = v.minDepth,
                              .maxDepth = v.maxDepth};
    fn(cmd).vkCmdSetViewport(native(cmd), 0, 1, &viewport);
}

void applyScissor(CommandList &cmd) {
    const Rect &r = cmd.internal.scissor;
    const VkRect2D scissor{.offset = {r.x, r.y}, .extent = {r.width, r.height}};
    fn(cmd).vkCmdSetScissor(native(cmd), 0, 1, &scissor);
}

// The state every rendering section starts with. The viewport and scissor
// cover the section until the user sets them (P103).
void openSection(CommandList &cmd, uint32 width, uint32 height) {
    cmd.internal.renderWidth = width;
    cmd.internal.renderHeight = height;
    cmd.internal.viewport = {.width = static_cast<float>(width), .height = static_cast<float>(height)};
    cmd.internal.scissor = {.width = width, .height = height};
    applyViewport(cmd);
    applyScissor(cmd);
    cmd.internal.stencilOverride = false;
    cmd.internal.rendering = true;
}

} // namespace

void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdBeginRendering"));
    constexpr size_t kMaxColors = std::size(CommandList{}.internal.colors);
    if (desc.colors.size() > kMaxColors) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("cmdBeginRendering: {} color attachments; at most {}", desc.colors.size(), kMaxColors));
    }
    if (const Validation::CheckResult r = Validation::checkRenderingAttachments(desc.colors.size(), desc.depth.has_value())) {
        reportError(ErrorCategory::eProgrammerError, *r);
    }
    const bool transitions = state.layoutMode == LayoutMode::eTransitions;
    std::vector<VkImageMemoryBarrier2> barriers;
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(desc.colors.size());
    uint32 width = UINT32_MAX;
    uint32 height = UINT32_MAX;
    uint32 layers = UINT32_MAX;
    const auto cover = [&](const TextureView &view) {
        width = std::min(width, view.width);
        height = std::min(height, view.height);
        layers = std::min(layers, view.internal.layerCount);
    };

    for (size_t i = 0; i < desc.colors.size(); ++i) {
        const ColorAttachment &a = desc.colors[i];
        EQ_VK_CHECK_ALIVE(&state, a.view.internal.textureId, "texture");
#if EQUINE_VALIDATION
        EQ_VALIDATE(Validation::checkDiscarded(state.placements, a.view.internal.textureId, "cmdBeginRendering"));
#endif
        const auto resting = static_cast<VkImageLayout>(a.view.internal.restingLayout);
        const VkImageLayout layout = transitions ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (layout != resting) {
            // Full src half, exact dst half (P68). Contents the load op does not
            // read need no preserving (evident, P36).
            barriers.push_back(imageBarrier(
                a.view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         .srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT,
                         .dstStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         .oldLayout = a.load == LoadOp::eLoad ? resting : VK_IMAGE_LAYOUT_UNDEFINED,
                         .newLayout = layout}));
        }
        VkClearValue clear{};
        clear.color = clearColorOf(a.view.format, a.clear);
        colors.push_back({.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                          .imageView = attachmentViewOf(a.view),
                          .imageLayout = layout,
                          .loadOp = loadOf(a.load),
                          .storeOp = storeOf(a.store, false),
                          .clearValue = clear});
        cover(a.view);
        cmd.internal.colors[i] = a.view;
        cmd.internal.resolves[i] = {};
        if (a.resolveTo.format != Format::eUndefined) {
            EQ_VK_CHECK_ALIVE(&state, a.resolveTo.internal.textureId, "texture");
#if EQUINE_VALIDATION
            EQ_VALIDATE(Validation::checkDiscarded(state.placements, a.resolveTo.internal.textureId, "cmdBeginRendering"));
#endif
            EQ_VALIDATE(Validation::checkResolve(resolveCheckOf(state, a.view, a.resolveTo, a.resolveMode),
                                                 "cmdBeginRendering"));
            barriers.push_back(resolveTargetIn(a.resolveTo, layout));
            colors.back().resolveMode = resolveModeOf(a.resolveMode);
            colors.back().resolveImageView = attachmentViewOf(a.resolveTo);
            colors.back().resolveImageLayout = layout;
            cmd.internal.resolves[i] = a.resolveTo;
        }
    }
    cmd.internal.colorCount = static_cast<uint32>(desc.colors.size());

    VkRenderingAttachmentInfo depthInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencilInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool depthAspect = false;
    bool stencilAspect = false;
    cmd.internal.depth = {};
    cmd.internal.depthReadOnly = false;
    if (desc.depth) {
        const DepthAttachment &d = *desc.depth;
        EQ_VK_CHECK_ALIVE(&state, d.view.internal.textureId, "texture");
#if EQUINE_VALIDATION
        EQ_VALIDATE(Validation::checkDiscarded(state.placements, d.view.internal.textureId, "cmdBeginRendering"));
#endif
        depthAspect = (d.view.internal.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        stencilAspect = (d.view.internal.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const auto resting = static_cast<VkImageLayout>(d.view.internal.restingLayout);
        const VkImageLayout layout =
            depthSectionLayout(state.layoutMode, d.depthReadOnly, d.stencilReadOnly, stencilAspect);
        if (layout != resting) {
            const bool keep =
                (depthAspect && d.load == LoadOp::eLoad) || (stencilAspect && d.stencilLoad == LoadOp::eLoad);
            barriers.push_back(imageBarrier(
                d.view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         .srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT,
                         .dstStage = kDepthStages,
                         .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         .oldLayout = keep ? resting : VK_IMAGE_LAYOUT_UNDEFINED,
                         .newLayout = layout}));
        }
        VkClearValue clear{};
        clear.depthStencil = {.depth = d.clear.depth, .stencil = d.clear.stencil};
        depthInfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                     .imageView = attachmentViewOf(d.view),
                     .imageLayout = layout,
                     .loadOp = loadOf(d.load),
                     .storeOp = storeOf(d.store, d.depthReadOnly),
                     .clearValue = clear};
        cmd.internal.depthResolve = {};
        if (d.resolveTo.format != Format::eUndefined) {
            EQ_VK_CHECK_ALIVE(&state, d.resolveTo.internal.textureId, "texture");
#if EQUINE_VALIDATION
            EQ_VALIDATE(Validation::checkDiscarded(state.placements, d.resolveTo.internal.textureId, "cmdBeginRendering"));
#endif
            EQ_VALIDATE(Validation::checkResolve(resolveCheckOf(state, d.view, d.resolveTo, d.resolveMode),
                                                 "cmdBeginRendering"));
            const VkImageLayout resolveLayout =
                state.layoutMode == LayoutMode::eTransitions ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            barriers.push_back(resolveTargetIn(d.resolveTo, resolveLayout));
            depthInfo.resolveMode = resolveModeOf(d.resolveMode);
            depthInfo.resolveImageView = attachmentViewOf(d.resolveTo);
            depthInfo.resolveImageLayout = resolveLayout;
            cmd.internal.depthResolve = d.resolveTo;
        }
        stencilInfo = depthInfo;
        stencilInfo.loadOp = loadOf(d.stencilLoad);
        stencilInfo.storeOp = storeOf(d.stencilStore, d.stencilReadOnly);
        cover(d.view);
        cmd.internal.depth = d.view;
        cmd.internal.depthLayout = static_cast<uint32>(layout);
        cmd.internal.depthReadOnly = d.depthReadOnly;
    }
    pipelineBarrier(cmd, barriers);

    const VkRenderingInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {width, height}},
        .layerCount = layers,
        .colorAttachmentCount = static_cast<uint32>(colors.size()),
        .pColorAttachments = colors.data(),
        .pDepthAttachment = depthAspect ? &depthInfo : nullptr,
        .pStencilAttachment = stencilAspect ? &stencilInfo : nullptr,
    };
    fn(cmd).vkCmdBeginRendering(native(cmd), &info);
    openSection(cmd, width, height);
}

void cmdBeginRendering(CommandList &cmd, RenderingSize size) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdBeginRendering"));
    EQ_VALIDATE(Validation::checkRenderingSize(size));
    cmd.internal.colorCount = 0;
    cmd.internal.depth = {};
    cmd.internal.depthResolve = {};
    cmd.internal.depthReadOnly = false;
    // Dynamic rendering takes zero attachments; the render area gives the size.
    const VkRenderingInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = {size.width, size.height}},
        .layerCount = 1,
    };
    fn(cmd).vkCmdBeginRendering(native(cmd), &info);
    openSection(cmd, size.width, size.height);
}

void cmdSetViewport(CommandList &cmd, const Viewport &viewport) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, true, "cmdSetViewport"));
    EQ_VALIDATE(Validation::checkViewport(viewport));
    cmd.internal.viewport = viewport;
    applyViewport(cmd);
}

void cmdSetScissor(CommandList &cmd, const Rect &scissor) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, true, "cmdSetScissor"));
    EQ_VALIDATE(Validation::checkScissor(scissor));
    cmd.internal.scissor = scissor;
    applyScissor(cmd);
}

void cmdSetStencilReference(CommandList &cmd, uint8 reference) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, true, "cmdSetStencilReference"));
    cmd.internal.stencilReference = reference;
    cmd.internal.stencilOverride = true;
    fn(cmd).vkCmdSetStencilReference(native(cmd), VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void cmdEndRendering(CommandList &cmd) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, true, "cmdEndRendering"));
    fn(cmd).vkCmdEndRendering(native(cmd));

    // Back to rest: exact src half, full dst half (P68).
    std::vector<VkImageMemoryBarrier2> barriers;
    const bool transitions = cmd.internal.device->layoutMode == LayoutMode::eTransitions;
    for (uint32 i = 0; i < cmd.internal.colorCount; ++i) {
        const TextureView &view = cmd.internal.colors[i];
        const VkImageLayout layout = transitions ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        const auto resting = static_cast<VkImageLayout>(view.internal.restingLayout);
        if (layout != resting) {
            barriers.push_back(imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                   .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                   .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                   .dstAccess = kAllAccess,
                                                   .oldLayout = layout,
                                                   .newLayout = resting}));
        }
    }
    const TextureView &depth = cmd.internal.depth;
    if (depth.internal.view != 0 && cmd.internal.depthLayout != depth.internal.restingLayout) {
        barriers.push_back(imageBarrier(depth, {.srcStage = kDepthStages,
                                                .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                .dstAccess = kAllAccess,
                                                .oldLayout = static_cast<VkImageLayout>(cmd.internal.depthLayout),
                                                .newLayout = static_cast<VkImageLayout>(depth.internal.restingLayout)}));
    }
    // Resolve targets back to rest: exact src half on the resolve, full dst half.
    const auto resolveBack = [&](const TextureView &view, VkImageLayout layout) {
        if (view.internal.view != 0 && layout != static_cast<VkImageLayout>(view.internal.restingLayout)) {
            barriers.push_back(imageBarrier(view, {.srcStage = kResolveStages,
                                                   .srcAccess = kResolveAccess,
                                                   .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                   .dstAccess = kAllAccess,
                                                   .oldLayout = layout,
                                                   .newLayout = static_cast<VkImageLayout>(view.internal.restingLayout)}));
        }
    };
    for (uint32 i = 0; i < cmd.internal.colorCount; ++i) {
        resolveBack(cmd.internal.resolves[i],
                    transitions ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
    }
    resolveBack(cmd.internal.depthResolve, transitions ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
    pipelineBarrier(cmd, barriers);
    for (uint32 i = 0; i < cmd.internal.colorCount; ++i) {
        cmd.internal.colors[i] = {};
        cmd.internal.resolves[i] = {};
    }
    cmd.internal.depthResolve = {};
    cmd.internal.colorCount = 0;
    cmd.internal.depth = {};
    cmd.internal.rendering = false;
    cmd.internal.depthReadOnly = false;
}

void cmdDrawMesh(CommandList &cmd, uint32 x, uint32 y, uint32 z) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkGroupCounts(x, y, z, true, "cmdDrawMesh"));
    fn(cmd).vkCmdDrawMeshTasksEXT(native(cmd), x, y, z);
}

void cmdSetIndexBuffer(CommandList &cmd, const Buffer &buffer, uint64 offset, IndexFormat format) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkIndexBuffer((buffer.usage & BufferUsage::eIndex) != BufferUsage::eNone, offset,
                                             format == IndexFormat::eUint16 ? 2 : 4, buffer.size));
    fn(cmd).vkCmdBindIndexBuffer(native(cmd), native(buffer), offset,
                                 format == IndexFormat::eUint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void cmdDrawIndexed(CommandList &cmd, uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset,
                    uint32 firstInstance) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    fn(cmd).vkCmdDrawIndexed(native(cmd), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void cmdDraw(CommandList &cmd, uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    // No RI section check: native rendering sections (escape hatch) may hold RI draws;
    // the validation layer reports a draw outside any section.
    fn(cmd).vkCmdDraw(native(cmd), vertexCount, instanceCount, firstVertex, firstInstance);
}

// ── Discard and clears (P65, P69) ────────────────────────────────────────────

void cmdDiscard(CommandList &cmd, const TextureView &view) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, view.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdDiscard"));
    // Full scope on both halves (P68, P69); UNDEFINED drops contents and metadata.
    const VkImageMemoryBarrier2 barrier =
        imageBarrier(view, {.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = static_cast<VkImageLayout>(view.internal.restingLayout)});
    pipelineBarrier(cmd, {&barrier, 1});
#if EQUINE_VALIDATION
    Validation::markDiscarded(state.placements, view.internal.textureId);
#endif
}

void cmdClearTexture(CommandList &cmd, const TextureView &view, const ClearValue &value) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, view.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdClearTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, view.internal.textureId, "cmdClearTexture"));
#endif
    // GENERAL textures are cleared in place; others switch to the transfer
    // layout and back (mode T, P68).
    const auto resting = static_cast<VkImageLayout>(view.internal.restingLayout);
    const VkImageLayout layout =
        resting == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (layout != resting) {
        const VkImageMemoryBarrier2 into = imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                               .srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                               .dstStage = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                                               .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                               .oldLayout = resting,
                                                               .newLayout = layout});
        pipelineBarrier(cmd, {&into, 1});
    }
    const VkImageSubresourceRange range = rangeOf(view);
    const auto image = reinterpret_cast<VkImage>(view.internal.image);
    if ((view.internal.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        const VkClearColorValue color = clearColorOf(view.format, value);
        fn(cmd).vkCmdClearColorImage(native(cmd), image, layout, &color, 1, &range);
    } else {
        const VkClearDepthStencilValue depthStencil{.depth = value.depth, .stencil = value.stencil};
        fn(cmd).vkCmdClearDepthStencilImage(native(cmd), image, layout, &depthStencil, 1, &range);
    }
    if (layout != resting) {
        const VkImageMemoryBarrier2 back = imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                                               .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                               .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                               .dstAccess = kAllAccess,
                                                               .oldLayout = layout,
                                                               .newLayout = resting});
        pipelineBarrier(cmd, {&back, 1});
    }
}

void cmdClearBuffer(CommandList &cmd, const Buffer &buffer, uint32 value, uint64 offset, uint64 size) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdClearBuffer"));
    if ((buffer.usage & BufferUsage::eCopyDst) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError, "cmdClearBuffer: the buffer needs BufferUsage::eCopyDst");
    }
    EQ_VALIDATE(Validation::checkClearRange(offset, size, buffer.size));
    fn(cmd).vkCmdFillBuffer(native(cmd), native(buffer), offset, size == kWholeSize ? VK_WHOLE_SIZE : size, value);
}

void cmdWriteSmallData(CommandList &cmd, const Buffer &buffer, uint64 offset, std::span<const std::byte> data) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdWriteSmallData"));
    EQ_VALIDATE(Validation::checkSmallWrite((buffer.usage & BufferUsage::eCopyDst) != BufferUsage::eNone, offset,
                                            data.size(), buffer.size));
    fn(cmd).vkCmdUpdateBuffer(native(cmd), native(buffer), offset, data.size(), data.data());
}

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

void cmdWriteTimestamp(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type == QueryType::eTimestamp, "cmdWriteTimestamp"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdWriteTimestamp: the query"));
    fn(cmd).vkCmdWriteTimestamp2(native(cmd), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, native(pool), index);
}

void cmdResetQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdResetQueries"));
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "cmdResetQueries: the query range"));
    fn(cmd).vkCmdResetQueryPool(native(cmd), native(pool), first, count);
}

void cmdResolveQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count, const Buffer &buffer,
                       uint64 offset) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VK_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdResolveQueries"));
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "cmdResolveQueries: the query range"));
    const uint64 stride = queryResultSize(*cmd.internal.device, pool.type);
    EQ_VALIDATE(Validation::checkRange(offset, uint64{count} * stride, buffer.size, 8,
                                       "cmdResolveQueries: the results in the buffer"));
    if ((buffer.usage & BufferUsage::eCopyDst) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError, "cmdResolveQueries: the buffer needs BufferUsage::eCopyDst");
    }
    // 64-bit values, waiting for each query (P71 (1)); never resets them.
    fn(cmd).vkCmdCopyQueryPoolResults(native(cmd), native(pool), first, count, native(buffer), offset, stride,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

void cmdBeginQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type != QueryType::eTimestamp, "cmdBeginQuery"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdBeginQuery: the query"));
    EQ_VALIDATE(Validation::checkQueryNesting(pool.type, cmd.internal.openStatistics));
    if (pool.type == QueryType::ePipelineStatistics || pool.type == QueryType::eMeshPipelineStatistics) {
        ++cmd.internal.openStatistics;
    }
    // Occlusion counts are exact (occlusionQueryPrecise), as on D3D12.
    const VkQueryControlFlags flags = pool.type == QueryType::eOcclusion ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    fn(cmd).vkCmdBeginQuery(native(cmd), native(pool), index, flags);
}

void cmdEndQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type != QueryType::eTimestamp, "cmdEndQuery"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdEndQuery: the query"));
    if ((pool.type == QueryType::ePipelineStatistics || pool.type == QueryType::eMeshPipelineStatistics) &&
        cmd.internal.openStatistics > 0) {
        --cmd.internal.openStatistics;
    }
    fn(cmd).vkCmdEndQuery(native(cmd), native(pool), index);
}

// ── Copies (P47) ─────────────────────────────────────────────────────────────

namespace {

uint32 copyBytesOf(const TextureView &view) {
    const uint32 aspects = view.internal.aspectMask;
    if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        return 0; // one aspect at a time
    }
    return copyBytesPerTexel(view.format, aspects == VK_IMAGE_ASPECT_STENCIL_BIT ? Aspect::eStencil : Aspect::eDefault);
}

uint64 roundUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

void checkCopy([[maybe_unused]] const TextureView &view, [[maybe_unused]] bool bufferCopy, [[maybe_unused]] TextureUsage usage, [[maybe_unused]] std::string_view usageName, [[maybe_unused]] std::string_view call) {
    EQ_VALIDATE(Validation::checkCopyView(view.internal.mipCount, view.samples, bufferCopy, copyBytesOf(view),
                                          (view.internal.textureUsage & usage) != TextureUsage::eNone, usageName,
                                          call));
}

// The image subresource of a one-mip view; a 3D texture's slices are its
// depth (sliceOffset and sliceExtent), not array layers.
VkImageSubresourceLayers layersOf(const TextureView &view) {
    return {.aspectMask = view.internal.aspectMask,
            .mipLevel = view.internal.baseMip,
            .baseArrayLayer = view.internal.volume ? 0 : view.internal.baseLayer,
            .layerCount = view.internal.volume ? 1 : view.internal.layerCount};
}

VkOffset3D sliceOffset(const TextureView &view) {
    return {0, 0, view.internal.volume ? static_cast<int32_t>(view.internal.baseLayer) : 0};
}

VkExtent3D sliceExtent(const TextureView &view) {
    return {view.width, view.height, view.internal.volume ? view.internal.layerCount : 1};
}

// The layout a copy uses: in place for textures resting in GENERAL, else the
// transfer layout (mode T), with RI's exact/full switches around it (P68).
VkImageLayout copyLayoutFor(const TextureView &view, bool destination) {
    const auto resting = static_cast<VkImageLayout>(view.internal.restingLayout);
    if (resting == VK_IMAGE_LAYOUT_GENERAL) {
        return resting;
    }
    return destination ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

void switchForCopy(std::vector<VkImageMemoryBarrier2> &barriers, const TextureView &view, bool destination,
                   bool back) {
    const auto resting = static_cast<VkImageLayout>(view.internal.restingLayout);
    const VkImageLayout layout = copyLayoutFor(view, destination);
    if (layout == resting) {
        return;
    }
    const VkAccessFlags2 copyAccess = destination ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
    if (!back) {
        // A copy destination's old contents are kept: the copy may cover only
        // part of what other views see.
        barriers.push_back(imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                               .srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                               .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
                                               .dstAccess = copyAccess,
                                               .oldLayout = resting,
                                               .newLayout = layout}));
    } else {
        barriers.push_back(imageBarrier(view, {.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
                                               .srcAccess = destination ? copyAccess : VK_ACCESS_2_NONE,
                                               .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                               .dstAccess = kAllAccess,
                                               .oldLayout = layout,
                                               .newLayout = resting}));
    }
}

void checkBufferCopyRange([[maybe_unused]] const Buffer &buffer, [[maybe_unused]] uint64 offset, [[maybe_unused]] const CopyLayout &layout, [[maybe_unused]] BufferUsage usage, [[maybe_unused]] std::string_view call) {
    EQ_VALIDATE(Validation::checkRange(offset, layout.size, buffer.size, layout.alignment,
                                       std::format("{}: the texels in the buffer", call)));
    if ((buffer.usage & usage) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the buffer needs BufferUsage::{}", call,
                                usage == BufferUsage::eCopySrc ? "eCopySrc" : "eCopyDst"));
    }
}

VkBufferImageCopy2 regionOf(const TextureView &view, uint64 offset, const CopyLayout &layout) {
    return {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset = offset,
            .bufferRowLength = static_cast<uint32>(layout.rowPitch / layout.bytesPerTexel),
            .bufferImageHeight = static_cast<uint32>(layout.layerPitch / layout.rowPitch),
            .imageSubresource = layersOf(view),
            .imageOffset = sliceOffset(view),
            .imageExtent = sliceExtent(view)};
}

} // namespace

CopyLayout copyLayout(Device &device, const TextureView &view) {
    const DeviceState &state = *device.internal.state;
    CopyLayout layout;
    layout.width = view.width;
    layout.height = view.height;
    layout.layers = view.internal.layerCount;
    layout.bytesPerTexel = copyBytesOf(view);
    if (layout.bytesPerTexel == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    "copyLayout: the view has both depth and stencil; copies take one aspect (P47)");
    }
    // Row pitch: whole texels, on the driver's optimal pitch; layers on the
    // optimal offset alignment, so every layer starts aligned.
    const uint64 texel = layout.bytesPerTexel;
    layout.alignment = std::max<uint64>({state.copyOffsetAlignment, texel, 4});
    layout.rowPitch = roundUp(roundUp(uint64{layout.width} * texel, state.copyRowPitchAlignment), texel);
    const uint64 layerBytes = layout.rowPitch * layout.height;
    // 3D slices follow each other directly, as D3D12 requires (P102).
    layout.layerPitch = view.internal.volume ? layerBytes : roundUp(layerBytes, layout.alignment);
    // layerPitch must be whole rows for bufferImageHeight.
    while (layout.layerPitch % layout.rowPitch != 0) {
        layout.layerPitch += layout.alignment;
    }
    layout.size = layout.layerPitch * (layout.layers - 1) + layerBytes;
    return layout;
}

void cmdCopyBufferToTexture(CommandList &cmd, const Buffer &src, uint64 srcOffset, const TextureView &dst) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, src.internal.id, "buffer");
    EQ_VK_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyBufferToTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyBufferToTexture"));
#endif
    checkCopy(dst, true, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyBufferToTexture");
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, dst);
    checkBufferCopyRange(src, srcOffset, layout, BufferUsage::eCopySrc, "cmdCopyBufferToTexture");
    std::vector<VkImageMemoryBarrier2> barriers;
    switchForCopy(barriers, dst, true, false);
    pipelineBarrier(cmd, barriers);
    const VkBufferImageCopy2 region = regionOf(dst, srcOffset, layout);
    const VkCopyBufferToImageInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                                        .srcBuffer = native(src),
                                        .dstImage = reinterpret_cast<VkImage>(dst.internal.image),
                                        .dstImageLayout = copyLayoutFor(dst, true),
                                        .regionCount = 1,
                                        .pRegions = &region};
    fn(cmd).vkCmdCopyBufferToImage2(native(cmd), &info);
    barriers.clear();
    switchForCopy(barriers, dst, true, true);
    pipelineBarrier(cmd, barriers);
}

void cmdCopyTextureToBuffer(CommandList &cmd, const TextureView &src, const Buffer &dst, uint64 dstOffset) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_VK_CHECK_ALIVE(&state, dst.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyTextureToBuffer"));
    checkCopy(src, true, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTextureToBuffer");
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, src);
    checkBufferCopyRange(dst, dstOffset, layout, BufferUsage::eCopyDst, "cmdCopyTextureToBuffer");
    std::vector<VkImageMemoryBarrier2> barriers;
    switchForCopy(barriers, src, false, false);
    pipelineBarrier(cmd, barriers);
    const VkBufferImageCopy2 region = regionOf(src, dstOffset, layout);
    const VkCopyImageToBufferInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                        .srcImage = reinterpret_cast<VkImage>(src.internal.image),
                                        .srcImageLayout = copyLayoutFor(src, false),
                                        .dstBuffer = native(dst),
                                        .regionCount = 1,
                                        .pRegions = &region};
    fn(cmd).vkCmdCopyImageToBuffer2(native(cmd), &info);
    barriers.clear();
    switchForCopy(barriers, src, false, true);
    pipelineBarrier(cmd, barriers);
}

void cmdCopyTexture(CommandList &cmd, const TextureView &src, const TextureView &dst) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_VK_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyTexture"));
#endif
    checkCopy(src, false, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTexture");
    checkCopy(dst, false, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyTexture");
    EQ_VALIDATE(Validation::checkCopyTextureMatch(src.width, src.height, src.internal.layerCount, src.samples,
                                                  copyBytesOf(src), dst.width, dst.height, dst.internal.layerCount,
                                                  dst.samples, copyBytesOf(dst)));
    std::vector<VkImageMemoryBarrier2> barriers;
    switchForCopy(barriers, src, false, false);
    switchForCopy(barriers, dst, true, false);
    pipelineBarrier(cmd, barriers);
    const VkImageCopy2 region{.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
                              .srcSubresource = layersOf(src),
                              .srcOffset = sliceOffset(src),
                              .dstSubresource = layersOf(dst),
                              .dstOffset = sliceOffset(dst),
                              // A 3D side copies as many slices as the other side has layers.
                              .extent = {src.width, src.height,
                                         src.internal.volume || dst.internal.volume ? src.internal.layerCount : 1}};
    const VkCopyImageInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
                                .srcImage = reinterpret_cast<VkImage>(src.internal.image),
                                .srcImageLayout = copyLayoutFor(src, false),
                                .dstImage = reinterpret_cast<VkImage>(dst.internal.image),
                                .dstImageLayout = copyLayoutFor(dst, true),
                                .regionCount = 1,
                                .pRegions = &region};
    fn(cmd).vkCmdCopyImage2(native(cmd), &info);
    barriers.clear();
    switchForCopy(barriers, src, false, true);
    switchForCopy(barriers, dst, true, true);
    pipelineBarrier(cmd, barriers);
}

// ── Resolve (P52, P76) ───────────────────────────────────────────────────────

// An empty rendering section whose attachment resolves into `dst`: Vulkan's
// resolve command handles color averages only, sections handle every mode.
void cmdResolve(CommandList &cmd, const TextureView &src, const TextureView &dst, ResolveMode mode) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdResolve"));
    RenderingDesc desc;
    if (isDepthFormat(src.format)) {
        desc.depth = DepthAttachment{.view = src, .resolveTo = dst, .resolveMode = mode};
    } else {
        desc.colors = {{.view = src, .resolveTo = dst, .resolveMode = mode}};
    }
    cmdBeginRendering(cmd, desc);
    cmdEndRendering(cmd);
}

// ── Native sections (P27, P70) ───────────────────────────────────────────────

NativeCommands beginNative(CommandList &cmd) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    if (cmd.internal.native) {
        reportError(ErrorCategory::eProgrammerError, "beginNative: a native section is already open");
    }
    cmd.internal.native = true;
    return {.commandBuffer = native(cmd), .functions = &fn(cmd)};
}

void endNative(CommandList &cmd, std::span<const NativeLayoutChange> changed) {
    EQ_VK_CHECK_THREAD(cmd.internal.device);
    if (!cmd.internal.native) {
        reportError(ErrorCategory::eProgrammerError, "endNative: no native section is open");
    }
    if (cmd.internal.rendering && !changed.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    "endNative: layouts cannot change inside a rendering section (P70 (2)); the list must be empty");
    }
    cmd.internal.native = false;
    // The textures native code left elsewhere go back to rest, full on both halves.
    std::vector<VkImageMemoryBarrier2> barriers;
    for (const NativeLayoutChange &c : changed) {
        const auto resting = static_cast<VkImageLayout>(c.view.internal.restingLayout);
        if (c.layout != resting) {
            barriers.push_back(imageBarrier(c.view, {.srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                     .srcAccess = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                     .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                     .dstAccess = kAllAccess,
                                                     .oldLayout = c.layout,
                                                     .newLayout = resting}));
        }
    }
    pipelineBarrier(cmd, barriers);
    // Everything RI set comes back (P70 (1)): heap sets, pipeline, root data, viewport.
    for (uint32 index = 0; index < 2; ++index) {
        if (cmd.internal.boundLayout[index] != 0) {
            bindHeap(cmd, index);
        }
    }
    for (uint32 index = 0; index < 2; ++index) {
        if (cmd.internal.boundPipeline[index] != 0) {
            fn(cmd).vkCmdBindPipeline(native(cmd), bindPointOf(index),
                                      reinterpret_cast<VkPipeline>(cmd.internal.boundPipeline[index]));
        }
    }
    if (cmd.internal.pipeline != 0) {
        const uint32 index = bindIndex(cmd.internal.graphics);
        if (cmd.internal.rootDataBytes > 0) {
            fn(cmd).vkCmdPushConstants(native(cmd), reinterpret_cast<VkPipelineLayout>(cmd.internal.boundLayout[index]),
                                       VK_SHADER_STAGE_ALL, 0, cmd.internal.rootDataBytes, cmd.internal.rootData);
        }
    }
    if (cmd.internal.rendering) {
        applyViewport(cmd);
        applyScissor(cmd);
        fn(cmd).vkCmdSetStencilReference(native(cmd), VK_STENCIL_FACE_FRONT_AND_BACK, cmd.internal.stencilReference);
    }
}

// ── Submission (P20) ─────────────────────────────────────────────────────────

TimelinePoint submit(Queue &queue, CommandList &cmd) {
    EQ_VK_CHECK_THREAD(queue.internal.device);
    DeviceState &state = *queue.internal.device;
    EQ_VK_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    const uint64 value = state.lastSignaled + 1;
    const VkCommandBufferSubmitInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                .commandBuffer = native(cmd)};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = state.timeline,
                                       .value = value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                   .commandBufferInfoCount = 1,
                                   .pCommandBufferInfos = &commandInfo,
                                   .signalSemaphoreInfoCount = 1,
                                   .pSignalSemaphoreInfos = &signal};
    checkQueueVk(state, state.functions.vkQueueSubmit2(state.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE),
                 "vkQueueSubmit2");
    state.lastSignaled = value;
    cmd.internal.lastSubmitted = value;
    return {value};
}

void wait(Queue &queue, TimelinePoint point) {
    DeviceState &state = *queue.internal.device;
    const VkSemaphoreWaitInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &state.timeline,
                                   .pValues = &point.value};
    checkQueueVk(state, state.functions.vkWaitSemaphores(state.device, &info, UINT64_MAX), "vkWaitSemaphores");
}

bool isReached(Queue &queue, TimelinePoint point) { return completedValue(*queue.internal.device) >= point.value; }

} // namespace RI::Vk
