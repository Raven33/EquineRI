// Indirect execution on Vulkan (P48).
//
// Owns: cmdExecuteIndirect (Level 1: vkCmdDraw*Indirect(Count), mesh and
// dispatch variants) and Level 2 (cmdExecuteIndirectWithRootData,
// indirectScratchRequirements) on VK_EXT_device_generated_commands: one
// indirect commands layout per (kind, root data size), a push-constant token
// then the draw or dispatch token.
//
// Keeps: the indirect layouts and their pipeline layouts, cached in
// DeviceState and destroyed with the device.
//
// Watch out: the driver's preprocess memory is the caller's scratch range
// (P31: RI allocates nothing); RADV allows it only in some GPU memory types,
// hence allocateMemory(..., &requirements). DGC is off on NVIDIA under GPU-AV
// (Device.cpp, P98 (5)).
//
// D3D12 counterpart: backends/d3d12/Indirect.cpp.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <format>

namespace RI::Vk {

namespace {

const VolkDeviceTable &fn(const CommandList &cmd) { return cmd.internal.device->functions; }

void checkIndirectCall([[maybe_unused]] const CommandList &cmd, [[maybe_unused]] IndirectKind kind, [[maybe_unused]] const Buffer &args, [[maybe_unused]] uint64 offset, [[maybe_unused]] uint32 maxCount, [[maybe_unused]] const Buffer *count, [[maybe_unused]] uint64 countOffset, [[maybe_unused]] uint32 stride, [[maybe_unused]] bool level2, [[maybe_unused]] std::string_view call) {
    EQ_VK_CHECK_ALIVE(cmd.internal.device, args.internal.id, "buffer");
    if (count != nullptr) {
        EQ_VK_CHECK_ALIVE(cmd.internal.device, count->internal.id, "buffer");
    }
    EQ_VALIDATE(Validation::checkIndirect(
        {.dispatch = kind == IndirectKind::eDispatch,
         .argsUsage = (args.usage & BufferUsage::eIndirect) != BufferUsage::eNone,
         .offset = offset,
         .stride = stride,
         .argsSize = args.size,
         .maxCount = maxCount,
         .hasCount = count != nullptr,
         .countUsage = count != nullptr && (count->usage & BufferUsage::eIndirect) != BufferUsage::eNone,
         .countOffset = countOffset,
         .countSize = count != nullptr ? count->size : 0,
         .countForDispatchAllowed = level2},
        call));
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, kind != IndirectKind::eDispatch, call));
}

uint64 layoutKey(IndirectKind kind, uint32 rootDataSize) {
    return (uint64{static_cast<uint32>(kind)} << 32) | rootDataSize;
}

// RI's pipeline layout for `rootDataSize`: the same set layouts and push
// constant range as every RI pipeline with that much root data.
VkPipelineLayout indirectPipelineLayout(DeviceState &state, uint32 rootDataSize) {
    auto found = state.indirectPipelineLayouts.find(rootDataSize);
    if (found != state.indirectPipelineLayouts.end()) {
        return found->second;
    }
    const VkDescriptorSetLayout setLayouts[] = {state.resourceSetLayout, state.samplerSetLayout};
    const VkPushConstantRange range{.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = rootDataSize};
    const VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 2,
                                          .pSetLayouts = setLayouts,
                                          .pushConstantRangeCount = rootDataSize > 0 ? 1u : 0u,
                                          .pPushConstantRanges = &range};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreatePipelineLayout(state.device, &info, nullptr, &layout), "vkCreatePipelineLayout");
    state.indirectPipelineLayouts.emplace(rootDataSize, layout);
    return layout;
}

VkShaderStageFlags stagesOf(IndirectKind kind) {
    switch (kind) {
    case IndirectKind::eDispatch: return VK_SHADER_STAGE_COMPUTE_BIT;
    case IndirectKind::eDrawMesh:
        return VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
}

VkIndirectCommandsLayoutEXT indirectLayout(DeviceState &state, IndirectKind kind, uint32 rootDataSize) {
    const uint64 key = layoutKey(kind, rootDataSize);
    auto found = state.indirectLayouts.find(key);
    if (found != state.indirectLayouts.end()) {
        return found->second;
    }
    VkIndirectCommandsLayoutTokenEXT tokens[2]{};
    uint32 tokenCount = 0;
    const uint32 rootBytes = (rootDataSize + 3) / 4 * 4;
    const VkIndirectCommandsPushConstantTokenEXT pushConstant{
        .updateRange = {.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = rootDataSize}};
    if (rootDataSize > 0) {
        tokens[tokenCount++] = {.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,
                                .type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT,
                                .data = {.pPushConstant = &pushConstant},
                                .offset = 0};
    }
    VkIndirectCommandsTokenTypeEXT type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
    switch (kind) {
    case IndirectKind::eDraw: type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT; break;
    case IndirectKind::eDrawIndexed: type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT; break;
    case IndirectKind::eDrawMesh: type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT; break;
    case IndirectKind::eDispatch: type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT; break;
    }
    tokens[tokenCount++] = {.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT, .type = type, .offset = rootBytes};
    const VkIndirectCommandsLayoutCreateInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,
        .shaderStages = stagesOf(kind),
        .indirectStride = indirectStride(kind, rootDataSize),
        .pipelineLayout = indirectPipelineLayout(state, rootDataSize),
        .tokenCount = tokenCount,
        .pTokens = tokens};
    VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateIndirectCommandsLayoutEXT(state.device, &info, nullptr, &layout),
            "vkCreateIndirectCommandsLayoutEXT");
    state.indirectLayouts.emplace(key, layout);
    return layout;
}

// What vkGetGeneratedCommandsMemoryRequirementsEXT reports for `pipeline`.
VkMemoryRequirements generatedCommandsRequirements(DeviceState &state, VkPipeline pipeline, IndirectKind kind,
                                                   uint32 rootDataSize, uint32 maxCount) {
    const VkGeneratedCommandsPipelineInfoEXT pipelineInfo{.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
                                                          .pipeline = pipeline};
    const VkGeneratedCommandsMemoryRequirementsInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT,
        .pNext = &pipelineInfo,
        .indirectCommandsLayout = indirectLayout(state, kind, rootDataSize),
        .maxSequenceCount = maxCount};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    state.functions.vkGetGeneratedCommandsMemoryRequirementsEXT(state.device, &info, &requirements);
    return requirements.memoryRequirements;
}

} // namespace

void destroyIndirectLayouts(DeviceState &state) {
    for (const auto &[key, layout] : state.indirectLayouts) {
        state.functions.vkDestroyIndirectCommandsLayoutEXT(state.device, layout, nullptr);
    }
    for (const auto &[size, layout] : state.indirectPipelineLayouts) {
        state.functions.vkDestroyPipelineLayout(state.device, layout, nullptr);
    }
    state.indirectLayouts.clear();
    state.indirectPipelineLayouts.clear();
}

void cmdExecuteIndirect(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
                        const Buffer *count, uint64 countOffset) {
    const uint32 stride = indirectStride(kind);
    checkIndirectCall(cmd, kind, args, offset, maxCount, count, countOffset, stride, false, "cmdExecuteIndirect");
    const VkCommandBuffer c = native(cmd);
    const VkBuffer countBuffer = count != nullptr ? native(*count) : VK_NULL_HANDLE;
    switch (kind) {
    case IndirectKind::eDraw:
        if (count != nullptr) {
            fn(cmd).vkCmdDrawIndirectCount(c, native(args), offset, countBuffer, countOffset, maxCount, stride);
        } else {
            fn(cmd).vkCmdDrawIndirect(c, native(args), offset, maxCount, stride);
        }
        break;
    case IndirectKind::eDrawIndexed:
        if (count != nullptr) {
            fn(cmd).vkCmdDrawIndexedIndirectCount(c, native(args), offset, countBuffer, countOffset, maxCount, stride);
        } else {
            fn(cmd).vkCmdDrawIndexedIndirect(c, native(args), offset, maxCount, stride);
        }
        break;
    case IndirectKind::eDrawMesh:
        if (count != nullptr) {
            fn(cmd).vkCmdDrawMeshTasksIndirectCountEXT(c, native(args), offset, countBuffer, countOffset, maxCount,
                                                       stride);
        } else {
            fn(cmd).vkCmdDrawMeshTasksIndirectEXT(c, native(args), offset, maxCount, stride);
        }
        break;
    case IndirectKind::eDispatch: fn(cmd).vkCmdDispatchIndirect(c, native(args), offset); break;
    }
}

MemoryRequirements indirectScratchRequirements(Device &device, const Pipeline &pipeline, IndirectKind kind,
                                               uint32 maxCount) {
    DeviceState &state = *device.internal.state;
    if (!state.generatedCommands) {
        reportError(ErrorCategory::eUnsupported,
                    "indirectScratchRequirements: this GPU has no indirect Level 2 (DeviceLimits::indirectRootData)");
    }
    EQ_VK_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    const VkMemoryRequirements requirements = generatedCommandsRequirements(
        state, reinterpret_cast<VkPipeline>(pipeline.internal.pipeline), kind, pipeline.rootDataSize, maxCount);
    // The types both the scratch range and a scratch buffer of that size allow,
    // so memory from allocateMemory(kind, size, &this) holds the buffer too.
    const MemoryRequirements buffer =
        bufferRequirements(device, {.size = std::max<uint64>(requirements.size, 4), .usage = BufferUsage::eIndirectScratch});
    const uint32 types = requirements.memoryTypeBits & buffer.internal.memoryTypes;
    return {.size = std::max<uint64>(requirements.size, buffer.size),
            .alignment = std::max<uint64>({requirements.alignment, buffer.alignment, 4}),
            .kinds = kindsOf(state, types),
            .internal = {.memoryTypes = types}};
}

void cmdExecuteIndirectWithRootData(CommandList &cmd, IndirectKind kind, const Buffer &records, uint64 offset,
                                    uint32 maxCount, const Buffer *scratch, uint64 scratchOffset, const Buffer *count,
                                    uint64 countOffset) {
    DeviceState &state = *cmd.internal.device;
    if (!state.generatedCommands) {
        reportError(ErrorCategory::eUnsupported,
                    "cmdExecuteIndirectWithRootData: this GPU has no indirect Level 2 "
                    "(DeviceLimits::indirectRootData; VK_EXT_device_generated_commands)");
    }
    const uint32 rootDataSize = cmd.internal.rootDataSize;
    const uint32 stride = indirectStride(kind, rootDataSize);
    checkIndirectCall(cmd, kind, records, offset, maxCount, count, countOffset, stride, true,
                      "cmdExecuteIndirectWithRootData");
    const VkPipeline pipeline = reinterpret_cast<VkPipeline>(cmd.internal.pipeline);
    const VkMemoryRequirements requirements =
        generatedCommandsRequirements(state, pipeline, kind, rootDataSize, maxCount);
    if (scratch != nullptr) {
        EQ_VK_CHECK_ALIVE(&state, scratch->internal.id, "buffer");
    }
    EQ_VALIDATE(Validation::checkIndirectScratch(
        {.requiredSize = requirements.size,
         .requiredAlignment = std::max<uint64>(requirements.alignment, 4),
         .hasScratch = scratch != nullptr,
         .usage = scratch != nullptr && (scratch->usage & BufferUsage::eIndirectScratch) != BufferUsage::eNone,
         .offset = scratchOffset,
         .scratchSize = scratch != nullptr ? scratch->size : 0,
         .address = scratch != nullptr ? scratch->gpuAddress + scratchOffset : 0},
        "cmdExecuteIndirectWithRootData"));
    // The bound pipeline (no execution set) is named through pNext.
    const VkGeneratedCommandsPipelineInfoEXT pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT, .pipeline = pipeline};
    const VkGeneratedCommandsInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
        .pNext = &pipelineInfo,
        .shaderStages = stagesOf(kind),
        .indirectCommandsLayout = indirectLayout(state, kind, rootDataSize),
        .indirectAddress = records.gpuAddress + offset,
        .indirectAddressSize = uint64{stride} * maxCount,
        .preprocessAddress = scratch != nullptr ? scratch->gpuAddress + scratchOffset : 0,
        .preprocessSize = requirements.size,
        .maxSequenceCount = maxCount,
        .sequenceCountAddress = count != nullptr ? count->gpuAddress + countOffset : 0,
    };
    fn(cmd).vkCmdExecuteGeneratedCommandsEXT(native(cmd), VK_FALSE, &info);
}

} // namespace RI::Vk
