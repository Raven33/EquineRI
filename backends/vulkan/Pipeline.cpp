// Pipelines on Vulkan (P38, P39, P44, P45, P67, P94).
//
// Owns: compute, graphics and mesh pipelines. The pipeline layout comes from
// the shaders (SPIRV-Reflect): the device's two heap set layouts and one
// push-constant range of exactly the root data size. Rendering formats come
// from the description (dynamic rendering).
//
// Watch out: the stencil reference is dynamic state that cmdSetPipeline sets
// from the description (P104); root data must agree between stages. In validation builds the
// pixel shader's inputs are matched to the outputs of the stage before it
// (geometry, domain, vertex or mesh) by DXC's interface names and locations
// (P101).
//
// D3D12 counterpart: backends/d3d12/Pipeline.cpp.

#include "DeviceState.hpp"
#include "VkCheck.hpp"

#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Vk/Native.hpp>

#include <spirv_reflect.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace RI::Vk {

namespace {

struct ShaderInfo {
    uint32 rootDataSize = 0;
    bool hasRootData = false;
    // The values passed between stages (P101), in validation builds.
    std::vector<Validation::StageValue> inputs;
    std::vector<Validation::StageValue> outputs;
};

#if EQUINE_VALIDATION
// DXC names a stage's interface variables "in.var.<semantic>" and
// "out.var.<semantic>", and numbers their locations in declaration order.
// Built-ins (SV_Position) have no location; variables without such a name
// (another compiler, stripped names) cannot be matched and are skipped.
std::vector<Validation::StageValue> stageValues(SpvReflectInterfaceVariable *const *variables, uint32 count,
                                                std::string_view prefix) {
    std::vector<Validation::StageValue> values;
    for (uint32 i = 0; i < count; ++i) {
        const SpvReflectInterfaceVariable &v = *variables[i];
        const std::string_view name = v.name != nullptr ? v.name : "";
        if (v.built_in != -1 || !name.starts_with(prefix) || name.size() == prefix.size()) {
            continue;
        }
        values.push_back({.semantic = Validation::stageSemantic(name.substr(prefix.size())),
                          .slot = v.location,
                          // UINT32_MAX without a Component decoration (DXC writes none).
                          .component = v.component == UINT32_MAX ? 0 : v.component});
    }
    return values;
}
#endif

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
#if EQUINE_VALIDATION
    if (problem.empty()) {
        info.inputs = stageValues(module.input_variables, module.input_variable_count, "in.var.");
        info.outputs = stageValues(module.output_variables, module.output_variable_count, "out.var.");
    }
#endif

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

VkStencilOp stencilOpOf(StencilOp op) {
    switch (op) {
    case StencilOp::eKeep: return VK_STENCIL_OP_KEEP;
    case StencilOp::eZero: return VK_STENCIL_OP_ZERO;
    case StencilOp::eReplace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::eIncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::eDecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::eInvert: return VK_STENCIL_OP_INVERT;
    case StencilOp::eIncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::eDecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor blendFactorOf(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::eZero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::eOne: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::eSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::eOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::eDstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::eOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::eSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::eOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::eDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::eOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp blendOpOf(BlendOp op) {
    switch (op) {
    case BlendOp::eAdd: return VK_BLEND_OP_ADD;
    case BlendOp::eSubtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::eReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::eMin: return VK_BLEND_OP_MIN;
    case BlendOp::eMax: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkCompareOp compareOf(CompareOp op);

VkStencilOpState stencilFaceOf(const StencilState &stencil, const StencilFace &face) {
    return {.failOp = stencilOpOf(face.fail),
            .passOp = stencilOpOf(face.pass),
            .depthFailOp = stencilOpOf(face.depthFail),
            .compareOp = compareOf(face.compare),
            .compareMask = stencil.readMask,
            .writeMask = stencil.writeMask,
            .reference = stencil.reference};
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
    case Topology::eLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Topology::ePointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Topology::ePatchList: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
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
    if (!desc.name.empty()) {
        applyName(*device.internal.state, out, desc.name);
    }
    return out;
}

namespace {

// Stages the GPU may lack (P102), reported in every build.
void checkStageSupport(const Device &device, const GraphicsPipelineDesc &desc, const char *call) {
    if (!desc.geometry.empty() && !device.limits.geometryShaders) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no geometry shaders (DeviceLimits::geometryShaders)", call));
    }
    if ((!desc.hull.empty() || !desc.domain.empty()) && !device.limits.tessellation) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no tessellation (DeviceLimits::tessellation)", call));
    }
}

} // namespace

// Complete state up front (P38), no vertex input (P39), dynamic viewport and
// scissor (set by cmdBeginRendering), dynamic rendering formats.
Pipeline createGraphicsPipeline(Device &device, const GraphicsPipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    constexpr const char *kCall = "createGraphicsPipeline";
    const bool meshPipeline = !desc.mesh.empty();
    if (meshPipeline == !desc.vertex.empty() || (!desc.task.empty() && !meshPipeline)) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: give a vertex shader, or a mesh shader (with an optional task shader), not both",
                                kCall));
    }
    if (meshPipeline && !device.limits.meshShaders) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no mesh shaders (DeviceLimits::meshShaders)", kCall));
    }
    checkStageSupport(device, desc, kCall);
    EQ_VALIDATE(Validation::checkGraphicsStages(meshPipeline, !desc.hull.empty(), !desc.domain.empty(),
                                                !desc.geometry.empty(), desc.topology == Topology::ePatchList,
                                                desc.patchControlPoints));
    // Every stage, with its reflection; one root data block for the whole pipeline (P44).
    struct Stage {
        Bytecode code;
        SpvReflectShaderStageFlagBits reflectStage;
        VkShaderStageFlagBits stage;
        const char *name;
    };
    std::vector<Stage> stageList;
    if (meshPipeline) {
        if (!desc.task.empty()) {
            stageList.push_back({desc.task, SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_TASK_BIT_EXT, "task"});
        }
        stageList.push_back({desc.mesh, SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"});
    } else {
        stageList.push_back({desc.vertex, SPV_REFLECT_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_VERTEX_BIT, "vertex"});
        if (!desc.hull.empty()) {
            stageList.push_back({desc.hull, SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "hull"});
        }
        if (!desc.domain.empty()) {
            stageList.push_back({desc.domain, SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "domain"});
        }
        if (!desc.geometry.empty()) {
            stageList.push_back(
                {desc.geometry, SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"});
        }
    }
    if (!desc.pixel.empty()) {
        stageList.push_back({desc.pixel, SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, "pixel"});
    }
    uint32 rootDataSize = 0;
    const char *rootStage = nullptr;
    [[maybe_unused]] std::vector<Validation::StageValue> producerOutputs;
    [[maybe_unused]] const char *producer = "";
    for (const Stage &stage : stageList) {
        const ShaderInfo info = reflect(stage.code, stage.reflectStage, kCall, stage.name);
        if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            EQ_VALIDATE(Validation::checkStageLink(producerOutputs, info.inputs, producer, "location"));
        } else if (stage.stage != VK_SHADER_STAGE_TASK_BIT_EXT) {
            producerOutputs = info.outputs;
            producer = stage.name;
        }
        if (!info.hasRootData) {
            continue;
        }
        if (rootStage != nullptr && info.rootDataSize != rootDataSize) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("{}: the {} shader declares {} bytes of root data and the {} shader {}; "
                                    "every stage must declare the same root data (P44)",
                                    kCall, rootStage, rootDataSize, stage.name, info.rootDataSize));
        }
        rootDataSize = info.rootDataSize;
        rootStage = stage.name;
    }
    EQ_VALIDATE(Validation::checkGraphicsState(desc.colorFormats.size(), desc.blend.size(), desc.stencil.test,
                                               hasStencil(desc.depthFormat)));
    const VkPipelineLayout layout = createLayout(state, device.limits, rootDataSize, kCall);

    std::vector<VkShaderModule> modules;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const Stage &stage : stageList) {
        modules.push_back(createModule(state, stage.code));
        stages.push_back({.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = stage.stage,
                          .module = modules.back(),
                          .pName = "main"});
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
    const VkPipelineTessellationStateCreateInfo tessellation{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .patchControlPoints = desc.patchControlPoints};
    const DepthBias &bias = desc.depthBias;
    const bool biased = bias.constant != 0 || bias.slope != 0.0f;
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = desc.depthClamp ? VK_TRUE : VK_FALSE,
        .polygonMode = desc.fill == FillMode::eWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .cullMode = cullOf(desc.cull),
        .frontFace = desc.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = biased ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = static_cast<float>(bias.constant),
        .depthBiasClamp = bias.clamp,
        .depthBiasSlopeFactor = bias.slope,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.samples),
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        // Writing depth needs the test on: test with eAlways when only writing,
        // as on D3D12, so both backends write depth for the same descriptions.
        .depthTestEnable = desc.depth.test || desc.depth.write ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc.depth.test ? compareOf(desc.depth.compare) : VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = desc.stencil.test ? VK_TRUE : VK_FALSE,
        .front = stencilFaceOf(desc.stencil, desc.stencil.front),
        .back = stencilFaceOf(desc.stencil, desc.stencil.back),
    };
    // One blend state per color target; none given: written as is.
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        colorFormats.size(), {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT});
    for (size_t i = 0; i < desc.blend.size() && i < blendAttachments.size(); ++i) {
        const BlendState &b = desc.blend[i];
        blendAttachments[i] = {.blendEnable = b.enable ? VK_TRUE : VK_FALSE,
                               .srcColorBlendFactor = blendFactorOf(b.srcColor),
                               .dstColorBlendFactor = blendFactorOf(b.dstColor),
                               .colorBlendOp = blendOpOf(b.colorOp),
                               .srcAlphaBlendFactor = blendFactorOf(b.srcAlpha),
                               .dstAlphaBlendFactor = blendFactorOf(b.dstAlpha),
                               .alphaBlendOp = blendOpOf(b.alphaOp),
                               .colorWriteMask = static_cast<VkColorComponentFlags>(b.writeMask)};
    }
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<uint32>(blendAttachments.size()),
        .pAttachments = blendAttachments.data(),
    };
    // The stencil reference is set by cmdSetPipeline or cmdSetStencilReference (P104).
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = static_cast<uint32>(stages.size()),
        .pStages = stages.data(),
        // Mesh pipelines have no vertex input or input assembly.
        .pVertexInputState = meshPipeline ? nullptr : &vertexInput,
        .pInputAssemblyState = meshPipeline ? nullptr : &inputAssembly,
        .pTessellationState = desc.hull.empty() ? nullptr : &tessellation,
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
    out.internal.stencilReference = desc.stencil.reference;
    out.internal.id = EQ_VK_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(*device.internal.state, out, desc.name);
    }
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_VK_UNTRACK(&state, pipeline.internal.id);
    if (!pipeline.internal.imported) { // P77: an imported pipeline stays the user's
        state.functions.vkDestroyPipeline(state.device, native(pipeline).pipeline, nullptr);
        state.functions.vkDestroyPipelineLayout(state.device, native(pipeline).layout, nullptr);
    }
    pipeline = {};
}

// ── Import (P77) ─────────────────────────────────────────────────────────────

Pipeline importPipeline(Device &device, VkPipeline pipeline, VkPipelineLayout layout, bool graphics,
                        uint32 rootDataSize) {
    EQ_VK_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    Pipeline result;
    result.rootDataSize = rootDataSize;
    result.internal.pipeline = reinterpret_cast<uint64>(pipeline);
    result.internal.layout = reinterpret_cast<uint64>(layout);
    result.internal.graphics = graphics;
    result.internal.imported = true;
    result.internal.id = EQ_VK_TRACK(&state);
    return result;
}

} // namespace RI::Vk
