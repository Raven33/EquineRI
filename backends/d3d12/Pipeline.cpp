// Compute pipelines (P38, P44, P45, P67, P94). RI builds the root signature
// from the shader with its own DXIL reader: heap-indexing flags, and root
// constants at b0 space999 with exactly the size the shader declares (P44).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Reflection/Dxil.hpp>

#include <format>

namespace RI::D3D12 {

namespace {

ID3D12RootSignature *createRootSignature(DeviceState &state, const Reflection::DxilInfo &info) {
    const D3D12_ROOT_PARAMETER1 rootData{
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0, .RegisterSpace = 999, .Num32BitValues = (info.rootDataSize + 3) / 4},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    };
    // Heaps are always bound (P94), so the heap-indexing flags are always set.
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = {.NumParameters = info.hasRootData ? 1u : 0u,
                     .pParameters = &rootData,
                     .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                              D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED},
    };
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT serialized = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors);
    if (FAILED(serialized)) {
        const std::string message =
            errors != nullptr ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                              : std::string("no details");
        release(errors);
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createComputePipeline: building the root signature failed: {}", message));
    }
    release(errors);
    ID3D12RootSignature *rootSignature = nullptr;
    checkHr(state.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&rootSignature)),
            "CreateRootSignature");
    release(blob);
    return rootSignature;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const Reflection::DxilReadResult read = Reflection::readDxil(desc.shader);
    if (!read.error.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createComputePipeline: {} (the D3D12 backend takes DXIL)", read.error));
    }
    const Reflection::DxilInfo &info = read.info;
    if (!info.bindings.empty()) {
        const Reflection::DxilBinding &b = info.bindings.front();
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createComputePipeline: the shader declares a resource at register {}{}, space {}: "
                                "declared resources are not supported yet; reach resources through "
                                "ResourceDescriptorHeap[] (P30)",
                                "tucs"[static_cast<int>(b.resourceClass)], b.lowerBound, b.space));
    }
    if (info.rootDataSize > device.limits.maxRootDataSize) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("createComputePipeline: the shader's root data is {} bytes; D3D12 allows {} (P44)",
                                info.rootDataSize, device.limits.maxRootDataSize));
    }

    ID3D12RootSignature *rootSignature = createRootSignature(state, info);
    const D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{
        .pRootSignature = rootSignature,
        .CS = {.pShaderBytecode = desc.shader.data(), .BytecodeLength = desc.shader.size()},
    };
    ID3D12PipelineState *pipelineState = nullptr;
    checkHr(state.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)),
            "CreateComputePipelineState");

    Pipeline out;
    out.rootDataSize = info.hasRootData ? info.rootDataSize : 0;
    out.internal.pipelineState = pipelineState;
    out.internal.rootSignature = rootSignature;
    out.internal.id = EQ_DX_TRACK(&state);
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_DX_UNTRACK(&state, pipeline.internal.id);
    auto *pso = static_cast<ID3D12PipelineState *>(pipeline.internal.pipelineState);
    auto *rootSignature = static_cast<ID3D12RootSignature *>(pipeline.internal.rootSignature);
    release(pso);
    release(rootSignature);
    pipeline = {};
}

} // namespace RI::D3D12
