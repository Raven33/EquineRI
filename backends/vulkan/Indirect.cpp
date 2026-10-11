// Indirect execution (P48). Level 1: Vulkan's indirect draws and dispatches,
// with counts. Level 2: VK_EXT_device_generated_commands, a push-constant
// token (the root data) before the draw or dispatch token.

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

// A preprocess buffer of at least `size` bytes for this command list.
// `typeBits`: the memory types vkGetGeneratedCommandsMemoryRequirementsEXT allows.
const PreprocessBuffer &preprocessBuffer(DeviceState &state, CommandList &cmd, VkDeviceSize size, uint32 typeBits) {
    auto *extras = static_cast<CommandListExtras *>(cmd.internal.extras);
    if (extras == nullptr) {
        extras = new CommandListExtras;
        cmd.internal.extras = extras;
    }
    if (!extras->preprocess.empty() && extras->preprocess.back().size >= size) {
        return extras->preprocess.back();
    }
    // Earlier buffers stay alive: commands already recorded use them.
    PreprocessBuffer p;
    p.size = std::max<VkDeviceSize>(size, 64 * 1024);
    const VkBufferUsageFlags2CreateInfo usage{.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
                                              .usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT |
                                                       VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT};
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &usage, .size = p.size};
    checkVk(state.functions.vkCreateBuffer(state.device, &bufferInfo, nullptr, &p.buffer), "vkCreateBuffer (preprocess)");
    VkMemoryRequirements requirements{};
    state.functions.vkGetBufferMemoryRequirements(state.device, p.buffer, &requirements);
    // RI's GPU type when allowed, else the first device-local type allowed,
    // else any allowed type.
    const uint32 allowed = requirements.memoryTypeBits & typeBits;
    uint32 type = state.memoryTypes[kindIndex(MemoryKind::eGPU)];
    if ((allowed & (1u << type)) == 0) {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(state.physicalDevice, &memory);
        type = UINT32_MAX;
        for (uint32 i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i) {
            if ((allowed & (1u << i)) != 0 &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                type = i;
            }
        }
        for (uint32 i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i) {
            if ((allowed & (1u << i)) != 0) {
                type = i;
            }
        }
    }
    if (type == UINT32_MAX) {
        reportError(ErrorCategory::eUnsupported, "indirect Level 2: no memory type fits the preprocess buffer");
    }
    const VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                          .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .pNext = &flags,
                                         .allocationSize = requirements.size,
                                         .memoryTypeIndex = type};
    checkVk(state.functions.vkAllocateMemory(state.device, &allocInfo, nullptr, &p.memory),
            "vkAllocateMemory (preprocess)");
    checkVk(state.functions.vkBindBufferMemory(state.device, p.buffer, p.memory, 0), "vkBindBufferMemory (preprocess)");
    const VkBufferDeviceAddressInfo addressInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = p.buffer};
    p.address = state.functions.vkGetBufferDeviceAddress(state.device, &addressInfo);
    extras->preprocess.push_back(p);
    return extras->preprocess.back();
}

} // namespace

void releaseCommandListExtras(DeviceState &state, CommandListExtras *&extras, bool keepLargest) {
    if (extras == nullptr) {
        return;
    }
    const size_t keep = keepLargest && !extras->preprocess.empty() ? 1 : 0;
    while (extras->preprocess.size() > keep) {
        const PreprocessBuffer &p = extras->preprocess.front();
        state.functions.vkDestroyBuffer(state.device, p.buffer, nullptr);
        state.functions.vkFreeMemory(state.device, p.memory, nullptr);
        extras->preprocess.erase(extras->preprocess.begin());
    }
    if (!keepLargest) {
        delete extras;
        extras = nullptr;
    }
}

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

void cmdExecuteIndirectWithRootData(CommandList &cmd, IndirectKind kind, const Buffer &records, uint64 offset,
                                    uint32 maxCount, const Buffer *count, uint64 countOffset) {
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
    const VkIndirectCommandsLayoutEXT layout = indirectLayout(state, kind, rootDataSize);
    const VkGeneratedCommandsMemoryRequirementsInfoEXT requirementsInfo{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT,
        .indirectCommandsLayout = layout,
        .maxSequenceCount = maxCount};
    VkMemoryRequirements2 requirements{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    // The bound pipeline (no execution set) is named through pNext.
    const VkGeneratedCommandsPipelineInfoEXT pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,
        .pipeline = reinterpret_cast<VkPipeline>(cmd.internal.pipeline)};
    VkGeneratedCommandsMemoryRequirementsInfoEXT withPipeline = requirementsInfo;
    withPipeline.pNext = &pipelineInfo;
    state.functions.vkGetGeneratedCommandsMemoryRequirementsEXT(state.device, &withPipeline, &requirements);
    const VkDeviceSize preprocessSize = std::max<VkDeviceSize>(requirements.memoryRequirements.size, 1);
    const PreprocessBuffer &preprocess =
        preprocessBuffer(state, cmd, preprocessSize, requirements.memoryRequirements.memoryTypeBits);
    const VkGeneratedCommandsInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,
        .pNext = &pipelineInfo,
        .shaderStages = stagesOf(kind),
        .indirectCommandsLayout = layout,
        .indirectAddress = records.gpuAddress + offset,
        .indirectAddressSize = uint64{stride} * maxCount,
        .preprocessAddress = preprocess.address,
        .preprocessSize = preprocessSize,
        .maxSequenceCount = maxCount,
        .sequenceCountAddress = count != nullptr ? count->gpuAddress + countOffset : 0,
    };
    fn(cmd).vkCmdExecuteGeneratedCommandsEXT(native(cmd), VK_FALSE, &info);
}

} // namespace RI::Vk
