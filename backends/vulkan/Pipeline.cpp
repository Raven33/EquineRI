// Compute pipelines (P38, P44, P45, P67, P94). The pipeline layout comes from
// the shader: the device's heap set layouts (sets 0 and 1) and a push-constant
// range of exactly the size the shader's root data declares (P44).

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
};

// Reads what RI needs from SPIR-V (SPIRV-Reflect, P67) and rejects bindings RI
// does not set up. Every problem is reported as a readable error.
ShaderInfo reflect(Bytecode bytecode) {
    SpvReflectShaderModule module{};
    if (bytecode.empty() || bytecode.size() % 4 != 0 ||
        spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, bytecode.size(), bytecode.data(), &module) !=
            SPV_REFLECT_RESULT_SUCCESS) {
        reportError(ErrorCategory::eProgrammerError,
                    "createComputePipeline: the shader is not valid SPIR-V (the Vulkan backend takes SPIR-V)");
    }
    std::string problem;
    ShaderInfo info;

    if (spvReflectGetEntryPoint(&module, "main") == nullptr) {
        problem = "the shader has no entry point named \"main\"";
    } else if (module.shader_stage != SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
        problem = "the shader is not a compute shader";
    }

    // Root data (P45): at most one push-constant block.
    if (problem.empty() && module.push_constant_block_count > 1) {
        problem = "the shader declares more than one root data block (EQ_ROOT_DATA)";
    } else if (problem.empty() && module.push_constant_block_count == 1) {
        const SpvReflectBlockVariable &block = module.push_constant_blocks[0];
        info.rootDataSize = block.offset + block.size;
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
        reportError(ErrorCategory::eProgrammerError, std::format("createComputePipeline: {}", problem));
    }
    return info;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const ShaderInfo info = reflect(desc.shader);
    if (info.rootDataSize > device.limits.maxRootDataSize) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("createComputePipeline: the shader's root data is {} bytes; this GPU allows {} (P44)",
                                info.rootDataSize, device.limits.maxRootDataSize));
    }

    const VkDescriptorSetLayout setLayouts[] = {state.resourceSetLayout, state.samplerSetLayout};
    const VkPushConstantRange range{.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = info.rootDataSize};
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = setLayouts,
        .pushConstantRangeCount = info.rootDataSize > 0 ? 1u : 0u,
        .pPushConstantRanges = &range,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreatePipelineLayout(state.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

    const VkShaderModuleCreateInfo moduleInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc.shader.size(),
        .pCode = reinterpret_cast<const uint32 *>(desc.shader.data()),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(state.functions.vkCreateShaderModule(state.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
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

void destroyPipeline(Device &device, Pipeline &pipeline) {
    DeviceState &state = *device.internal.state;
    EQ_VK_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_VK_UNTRACK(&state, pipeline.internal.id);
    state.functions.vkDestroyPipeline(state.device, native(pipeline).pipeline, nullptr);
    state.functions.vkDestroyPipelineLayout(state.device, native(pipeline).layout, nullptr);
    pipeline = {};
}

} // namespace RI::Vk
