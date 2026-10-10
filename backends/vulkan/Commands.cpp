// Command lists, recording and submission (P20, P34, P36, P47, P56, P85).

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Vk/Native.hpp>

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
    return flags;
}

// Binds the heap's sets with the current pipeline layout. Pipelines with
// different root data sizes have incompatible layouts, so the sets are bound
// again whenever the layout changes (P70: RI's implicit state).
void bindHeap(CommandList &cmd) {
    if (cmd.internal.boundLayout == 0 || cmd.internal.heapResourceSet == 0) {
        return;
    }
    const VkDescriptorSet sets[] = {reinterpret_cast<VkDescriptorSet>(cmd.internal.heapResourceSet),
                                    reinterpret_cast<VkDescriptorSet>(cmd.internal.heapSamplerSet)};
    fn(cmd).vkCmdBindDescriptorSets(native(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
                                    reinterpret_cast<VkPipelineLayout>(cmd.internal.boundLayout), 0, 2, sets, 0,
                                    nullptr);
}

uint64 completedValue(DeviceState &state) {
    uint64 value = 0;
    checkVk(state.functions.vkGetSemaphoreCounterValue(state.device, state.timeline, &value),
            "vkGetSemaphoreCounterValue");
    return value;
}

} // namespace

// ── Command lists (P56) ──────────────────────────────────────────────────────

CommandList createCommandList(Device &device) {
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
    return cmd;
}

void destroyCommandList(Device &device, CommandList &cmd) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "destroying"));
    EQ_VK_UNTRACK(&state, cmd.internal.id);
    state.functions.vkDestroyCommandPool(state.device, reinterpret_cast<VkCommandPool>(cmd.internal.pool), nullptr);
    cmd = {};
}

void resetCommandList(CommandList &cmd) {
    DeviceState &state = *cmd.internal.device;
    EQ_VK_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "resetting"));
    checkVk(state.functions.vkResetCommandPool(state.device, reinterpret_cast<VkCommandPool>(cmd.internal.pool), 0),
            "vkResetCommandPool");
}

// ── Recording (P34: each call writes the native command at once) ─────────────

void cmdBegin(CommandList &cmd) {
    EQ_VK_CHECK_ALIVE(cmd.internal.device, cmd.internal.id, "command list");
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    checkVk(fn(cmd).vkBeginCommandBuffer(native(cmd), &info), "vkBeginCommandBuffer");
    cmd.internal.boundLayout = 0;
    cmd.internal.heapResourceSet = 0;
    cmd.internal.heapSamplerSet = 0;
    cmd.internal.rootDataSize = 0;
}

void cmdEnd(CommandList &cmd) {
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
    fn(cmd).vkCmdPipelineBarrier2(native(cmd), &dependency);
    checkVk(fn(cmd).vkEndCommandBuffer(native(cmd)), "vkEndCommandBuffer");
}

void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap) {
    EQ_VK_CHECK_ALIVE(cmd.internal.device, heap.internal.id, "descriptor heap");
    cmd.internal.heapResourceSet = heap.internal.resourceSet;
    cmd.internal.heapSamplerSet = heap.internal.samplerSet;
    bindHeap(cmd);
}

void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline) {
    EQ_VK_CHECK_ALIVE(cmd.internal.device, pipeline.internal.id, "pipeline");
    fn(cmd).vkCmdBindPipeline(native(cmd), VK_PIPELINE_BIND_POINT_COMPUTE, native(pipeline).pipeline);
    cmd.internal.rootDataSize = pipeline.rootDataSize;
    if (cmd.internal.boundLayout != pipeline.internal.layout) {
        cmd.internal.boundLayout = pipeline.internal.layout;
        bindHeap(cmd);
    }
}

void cmdSetRootData(CommandList &cmd, const void *data, uint32 size) {
    EQ_VALIDATE(Validation::checkRootDataSize(size, cmd.internal.rootDataSize));
    fn(cmd).vkCmdPushConstants(native(cmd), reinterpret_cast<VkPipelineLayout>(cmd.internal.boundLayout),
                               VK_SHADER_STAGE_ALL, 0, size, data);
}

void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z) { fn(cmd).vkCmdDispatch(native(cmd), x, y, z); }

// Coarse stage -> stage barrier (P36): every write before, every access after.
void cmdBarrier(CommandList &cmd, Stage before, Stage after) {
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = stagesOf(before),
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = stagesOf(after),
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    fn(cmd).vkCmdPipelineBarrier2(native(cmd), &dependency);
}

void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size) {
    EQ_VK_CHECK_ALIVE(cmd.internal.device, src.internal.id, "buffer");
    EQ_VK_CHECK_ALIVE(cmd.internal.device, dst.internal.id, "buffer");
    const VkBufferCopy region{.srcOffset = srcOffset, .dstOffset = dstOffset, .size = size};
    fn(cmd).vkCmdCopyBuffer(native(cmd), native(src), native(dst), 1, &region);
}

// ── Submission (P20) ─────────────────────────────────────────────────────────

TimelinePoint submit(Queue &queue, CommandList &cmd) {
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
    checkVk(state.functions.vkQueueSubmit2(state.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2");
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
    checkVk(state.functions.vkWaitSemaphores(state.device, &info, UINT64_MAX), "vkWaitSemaphores");
}

bool isReached(Queue &queue, TimelinePoint point) { return completedValue(*queue.internal.device) >= point.value; }

} // namespace RI::Vk
