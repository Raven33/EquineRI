// Compute pipelines (P38, P44, P45, P67, P94). RI builds the root signature
// from the shader with its own DXIL reader: heap-indexing flags, and root
// constants at b0 space999 with exactly the size the shader declares (P44).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Reflection/Dxil.hpp>

#include "Formats.hpp"

#include <climits>

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

// Reads one stage's DXIL and rejects what RI does not set up yet.
Reflection::DxilInfo reflectStage(Bytecode bytecode, const char *call) {
    const Reflection::DxilReadResult read = Reflection::readDxil(bytecode);
    if (!read.error.empty()) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: {} (the D3D12 backend takes DXIL)", call, read.error));
    }
    if (!read.info.bindings.empty()) {
        const Reflection::DxilBinding &b = read.info.bindings.front();
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the shader declares a resource at register {}{}, space {}: declared resources are "
                                "not supported yet; reach resources through ResourceDescriptorHeap[] (P30)",
                                call, "tucs"[static_cast<int>(b.resourceClass)], b.lowerBound, b.space));
    }
    return read.info;
}

void checkRootDataSize(const Device &device, uint32 size, const char *call) {
    if (size > device.limits.maxRootDataSize) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: the shader's root data is {} bytes; D3D12 allows {} (P44)", call, size,
                                device.limits.maxRootDataSize));
    }
}

D3D12_COMPARISON_FUNC compareOf(CompareOp op) {
    switch (op) {
    case CompareOp::eNever: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::eLess: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::eEqual: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::eLessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::eGreater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::eNotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::eGreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::eAlways: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const Reflection::DxilInfo info = reflectStage(desc.shader, "createComputePipeline");
    checkRootDataSize(device, info.rootDataSize, "createComputePipeline");

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

// Graphics pipelines (P38, P39): complete state up front, no input layout.
Pipeline createGraphicsPipeline(Device &device, const GraphicsPipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const char *call = "createGraphicsPipeline";
    Reflection::DxilInfo info = reflectStage(desc.vertex, call);
    if (!desc.pixel.empty()) {
        const Reflection::DxilInfo pixel = reflectStage(desc.pixel, call);
        if (info.hasRootData && pixel.hasRootData && info.rootDataSize != pixel.rootDataSize) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("{}: the vertex and pixel shaders declare different root data ({} and {} bytes)",
                                    call, info.rootDataSize, pixel.rootDataSize));
        }
        if (!info.hasRootData && pixel.hasRootData) {
            info.hasRootData = true;
            info.rootDataSize = pixel.rootDataSize;
        }
    }
    checkRootDataSize(device, info.rootDataSize, call);
    if (desc.colorFormats.size() > 8) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: at most 8 color formats", call));
    }

    ID3D12RootSignature *rootSignature = createRootSignature(state, info);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature;
    pso.VS = {desc.vertex.data(), desc.vertex.size()};
    if (!desc.pixel.empty()) {
        pso.PS = {desc.pixel.data(), desc.pixel.size()};
    }
    for (D3D12_RENDER_TARGET_BLEND_DESC &rt : pso.BlendState.RenderTarget) {
        rt = {.BlendEnable = FALSE,
              .LogicOpEnable = FALSE,
              .SrcBlend = D3D12_BLEND_ONE,
              .DestBlend = D3D12_BLEND_ZERO,
              .BlendOp = D3D12_BLEND_OP_ADD,
              .SrcBlendAlpha = D3D12_BLEND_ONE,
              .DestBlendAlpha = D3D12_BLEND_ZERO,
              .BlendOpAlpha = D3D12_BLEND_OP_ADD,
              .LogicOp = D3D12_LOGIC_OP_NOOP,
              .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL};
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                           .CullMode = desc.cull == CullMode::eFront  ? D3D12_CULL_MODE_FRONT
                                       : desc.cull == CullMode::eBack ? D3D12_CULL_MODE_BACK
                                                                      : D3D12_CULL_MODE_NONE,
                           .FrontCounterClockwise = desc.frontCounterClockwise ? TRUE : FALSE,
                           .DepthClipEnable = TRUE};
    // Writing depth needs the depth test on in D3D12; RI then compares with eAlways.
    const bool depthEnable = desc.depth.test || desc.depth.write;
    pso.DepthStencilState.DepthEnable = depthEnable ? TRUE : FALSE;
    pso.DepthStencilState.DepthWriteMask = desc.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = desc.depth.test ? compareOf(desc.depth.compare) : D3D12_COMPARISON_FUNC_ALWAYS;
    pso.PrimitiveTopologyType = desc.topology == Topology::eLineList   ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                : desc.topology == Topology::ePointList ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                                                        : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());
    for (size_t i = 0; i < desc.colorFormats.size(); ++i) {
        pso.RTVFormats[i] = dxgiFormat(desc.colorFormats[i]);
    }
    pso.DSVFormat = dxgiFormat(desc.depthFormat);
    pso.SampleDesc = {.Count = desc.samples, .Quality = 0};
    ID3D12PipelineState *pipelineState = nullptr;
    checkHr(state.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState)), "CreateGraphicsPipelineState");

    Pipeline out;
    out.rootDataSize = info.hasRootData ? info.rootDataSize : 0;
    out.internal.pipelineState = pipelineState;
    out.internal.rootSignature = rootSignature;
    out.internal.graphics = true;
    out.internal.depthWrite = desc.depth.write;
    out.internal.topology = desc.topology == Topology::eTriangleStrip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                            : desc.topology == Topology::eLineList    ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                            : desc.topology == Topology::ePointList   ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                                                      : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    out.internal.id = EQ_DX_TRACK(&state);
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_DX_UNTRACK(&state, pipeline.internal.id);
    auto *pso = static_cast<ID3D12PipelineState *>(pipeline.internal.pipelineState);
    auto *rootSignature = static_cast<ID3D12RootSignature *>(pipeline.internal.rootSignature);
    release(pso);
    release(rootSignature);
    pipeline = {};
}

} // namespace RI::D3D12
