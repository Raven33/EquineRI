// Compute and graphics pipelines (P38, P39, P44, P45, P67, P94). The pipeline
// layout comes from the shaders: the device's heap set layouts (sets 0 and 1)
// and a push-constant range of exactly the size the root data declares (P44).

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Vk/Native.hpp>

#include <spirv_reflect.h>

#include <format>
#include <string>
#include <vector>

namespace RI::Vk {

namespace {

struct ShaderInfo {
    uint32 rootDataSize = 0;
    bool hasRootData = false;
};

// Reads what RI needs from SPIR-V (SPIRV-Reflect, P67) and rejects bindings RI
// does not set up. Every problem is reported as a readable error.
ShaderInfo reflect(Bytecode bytecode, SpvReflectShaderStageFlagBits stage, const char *call, const char *stageName) {
    SpvReflectShaderModule module{};
    if (bytecode.empty() || bytecode.size() % 4 != 0 ||
        spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, bytecode.size(), bytecode.data(), &module) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the {} shader is not valid SPIR-V (the Vulkan backend takes SPIR-V)", call,
                                stageName));
    }
    std::string problem;
    ShaderInfo info;

    if (spvReflectGetEntryPoint(&module, "main") == nullptr) {
        problem = "the shader has no entry point named \"main\"";
    } else if (module.shader_stage != stage) {
        problem = std::format("the shader is not a {} shader", stageName);
    }

    // Root data (P45): at most one push-constant block.
    if (problem.empty() && module.push_constant_block_count > 1) {
        problem = "the shader declares more than one root data block (EQ_ROOT_DATA)";
    } else if (problem.empty() && module.push_constant_block_count == 1) {
        const SpvReflectBlockVariable &block = module.push_constant_blocks[0];
        info.rootDataSize = block.offset + block.size;
        info.hasRootData = true;
    }

    // Only the descriptor heaps: set 0 binding 0 (ResourceDescriptorHeap) and
    // set 1 binding 0 (SamplerDescriptorHeap with -fvk-bind-sampler-heap 0 1).
    for (uint32 i = 0; problem.empty() && i < module.descriptor_binding_count; ++i) {
        const SpvReflectDescriptorBinding &b = module.descriptor_bindings[i];
        const bool resourceHeap = b.set == 0 && b.binding == 0;
        const bool samplerHeap = b.set == 1 && b.binding == 0 && b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER;
        if (resourceHeap || samplerHeap) {
            continue;
        }
        if (b.set == 0 && b.binding == 1 && b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER) {
            problem = "SamplerDescriptorHeap is at set 0, binding 1 (DXC's default): compile the shader with "
                      "-fvk-bind-sampler-heap 0 1";
        } else {
            problem = std::format("the shader declares '{}' at set {}, binding {}: declared resources are not "
                                  "supported yet; reach resources through ResourceDescriptorHeap[] (P30)",
                                  b.name != nullptr ? b.name : "?", b.set, b.binding);
        }
    }
    spvReflectDestroyShaderModule(&module);
    if (!problem.empty()) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: {} shader: {}", call, stageName, problem));
    }
    return info;
}


// The pipeline layout for `rootDataSize` bytes of root data (P44).
VkPipelineLayout createLayout(DeviceState &state, const DeviceLimits &limits, uint32 rootDataSize, const char *call) {
    if (rootDataSize > limits.maxRootDataSize) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: the shader's root data is {} bytes; this GPU allows {} (P44)", call, rootDataSize,
                                limits.maxRootDataSize));
    }
    const VkDescriptorSetLayout setLayouts[] = {state.resourceSetLayout, state.samplerSetLayout};
    const VkPushConstantRange range{.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = rootDataSize};
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = setLayouts,
        .pushConstantRangeCount = rootDataSize > 0 ? 1u : 0u,
        .pPushConstantRanges = &range,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreatePipelineLayout(state.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    return layout;
}

VkShaderModule createModule(DeviceState &state, Bytecode bytecode) {
    const VkShaderModuleCreateInfo moduleInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytecode.size(),
        .pCode = reinterpret_cast<const uint32 *>(bytecode.data()),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateShaderModule(state.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkCompareOp compareOf(CompareOp op) {
    switch (op) {
    case CompareOp::eNever: return VK_COMPARE_OP_NEVER;
    case CompareOp::eLess: return VK_COMPARE_OP_LESS;
    case CompareOp::eEqual: return VK_COMPARE_OP_EQUAL;
    case CompareOp::eLessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::eGreater: return VK_COMPARE_OP_GREATER;
    case CompareOp::eNotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::eGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::eAlways: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkPrimitiveTopology topologyOf(Topology topology) {
    switch (topology) {
    case Topology::eTriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::eTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case Topology::eLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::ePointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags cullOf(CullMode cull) {
    switch (cull) {
    case CullMode::eNone: return VK_CULL_MODE_NONE;
    case CullMode::eFront: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::eBack: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const ShaderInfo info = reflect(desc.shader, SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT, "createComputePipeline", "compute");
    const VkPipelineLayout layout = createLayout(state, device.limits, info.rootDataSize, "createComputePipeline");
    const VkShaderModule module = createModule(state, desc.shader);
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        state.functions.vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    state.functions.vkDestroyShaderModule(state.device, module, nullptr);
    checkVk(result, "vkCreateComputePipelines");

    Pipeline out;
    out.rootDataSize = info.rootDataSize;
    out.internal.pipeline = reinterpret_cast<uint64>(pipeline);
    out.internal.layout = reinterpret_cast<uint64>(layout);
    out.internal.id = EQ_VK_TRACK(&state);
    return out;
}

// Complete state up front (P38), no vertex input (P39), dynamic viewport and
// scissor (set by cmdBeginRendering), dynamic rendering formats.
Pipeline createGraphicsPipeline(Device &device, const GraphicsPipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    constexpr const char *kCall = "createGraphicsPipeline";
    const ShaderInfo vertex = reflect(desc.vertex, SPV_REFLECT_SHADER_STAGE_VERTEX_BIT, kCall, "vertex");
    ShaderInfo pixel;
    if (!desc.pixel.empty()) {
        pixel = reflect(desc.pixel, SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT, kCall, "pixel");
    }
    // One root data block for the whole pipeline (P44).
    if (vertex.hasRootData && pixel.hasRootData && vertex.rootDataSize != pixel.rootDataSize) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the vertex shader declares {} bytes of root data and the pixel shader {}; "
                                "both stages must declare the same root data (P44)",
                                kCall, vertex.rootDataSize, pixel.rootDataSize));
    }
    const uint32 rootDataSize = vertex.hasRootData ? vertex.rootDataSize : pixel.rootDataSize;
    const VkPipelineLayout layout = createLayout(state, device.limits, rootDataSize, kCall);

    VkShaderModule modules[2] = {createModule(state, desc.vertex), VK_NULL_HANDLE};
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = modules[0],
         .pName = "main"},
        {},
    };
    uint32 stageCount = 1;
    if (!desc.pixel.empty()) {
        modules[1] = createModule(state, desc.pixel);
        stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                     .module = modules[1],
                     .pName = "main"};
        stageCount = 2;
    }

    std::vector<VkFormat> colorFormats;
    for (Format format : desc.colorFormats) {
        colorFormats.push_back(toVkFormat(format));
    }
    const VkFormat depthFormat = toVkFormat(desc.depthFormat);
    const VkPipelineRenderingCreateInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = static_cast<uint32>(colorFormats.size()),
        .pColorAttachmentFormats = colorFormats.data(),
        .depthAttachmentFormat = depthFormat,
        .stencilAttachmentFormat = hasStencil(desc.depthFormat) ? depthFormat : VK_FORMAT_UNDEFINED,
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topologyOf(desc.topology)};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cullOf(desc.cull),
        .frontFace = desc.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.samples),
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth.test && desc.depth.write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = compareOf(desc.depth.compare),
    };
    // No blending yet: every color target is written as is.
    const std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        colorFormats.size(), {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT});
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<uint32>(blendAttachments.size()),
        .pAttachments = blendAttachments.data(),
    };
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = stageCount,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        state.functions.vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    for (VkShaderModule module : modules) {
        state.functions.vkDestroyShaderModule(state.device, module, nullptr);
    }
    checkVk(result, "vkCreateGraphicsPipelines");

    Pipeline out;
    out.rootDataSize = rootDataSize;
    out.internal.pipeline = reinterpret_cast<uint64>(pipeline);
    out.internal.layout = reinterpret_cast<uint64>(layout);
    out.internal.graphics = true;
    out.internal.depthWrite = desc.depth.test && desc.depth.write;
    out.internal.id = EQ_VK_TRACK(&state);
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_VK_UNTRACK(&state, pipeline.internal.id);
    state.functions.vkDestroyPipeline(state.device, native(pipeline).pipeline, nullptr);
    state.functions.vkDestroyPipelineLayout(state.device, native(pipeline).layout, nullptr);
    pipeline = {};
}

} // namespace RI::Vk
