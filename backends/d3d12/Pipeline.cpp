// Compute pipelines (P38, P44, P45, P67, P94). RI builds the root signature
// from the shader with its own DXIL reader: heap-indexing flags, and root
// constants at b0 space999 with exactly the size the shader declares (P44).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Reflection/Dxil.hpp>

#include "Formats.hpp"

#include <climits>
#include <cstring>

#include <format>
#include <string>
#include <vector>

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

D3D12_STENCIL_OP stencilOpOf(StencilOp op) {
    switch (op) {
    case StencilOp::eKeep: return D3D12_STENCIL_OP_KEEP;
    case StencilOp::eZero: return D3D12_STENCIL_OP_ZERO;
    case StencilOp::eReplace: return D3D12_STENCIL_OP_REPLACE;
    case StencilOp::eIncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
    case StencilOp::eDecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
    case StencilOp::eInvert: return D3D12_STENCIL_OP_INVERT;
    case StencilOp::eIncrementWrap: return D3D12_STENCIL_OP_INCR;
    case StencilOp::eDecrementWrap: return D3D12_STENCIL_OP_DECR;
    }
    return D3D12_STENCIL_OP_KEEP;
}

D3D12_BLEND blendOf(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::eZero: return D3D12_BLEND_ZERO;
    case BlendFactor::eOne: return D3D12_BLEND_ONE;
    case BlendFactor::eSrcColor: return D3D12_BLEND_SRC_COLOR;
    case BlendFactor::eOneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::eDstColor: return D3D12_BLEND_DEST_COLOR;
    case BlendFactor::eOneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::eSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::eOneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::eDstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::eOneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

// Alpha factors must not name a color: D3D12 rejects *_COLOR in alpha slots,
// and for alpha they mean the same as the *_ALPHA factors.
D3D12_BLEND alphaBlendOf(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::eSrcColor: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::eOneMinusSrcColor: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::eDstColor: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::eOneMinusDstColor: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return blendOf(factor);
    }
}

D3D12_BLEND_OP blendOpOf(BlendOp op) {
    switch (op) {
    case BlendOp::eAdd: return D3D12_BLEND_OP_ADD;
    case BlendOp::eSubtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::eReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::eMin: return D3D12_BLEND_OP_MIN;
    case BlendOp::eMax: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
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
// A pipeline-state stream subobject (mesh pipelines): its type, then its
// value, aligned as D3D12 reads the stream.
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void *) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};

struct MeshStream {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rootSignature;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> task;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> mesh;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> pixel;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sampleMask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depthStencil;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> renderTargets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depthFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sampleDesc;
};

Pipeline createGraphicsPipeline(Device &device, const GraphicsPipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const char *call = "createGraphicsPipeline";
    const bool meshPipeline = !desc.mesh.empty();
    if (meshPipeline == !desc.vertex.empty() || (!desc.task.empty() && !meshPipeline)) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: give a vertex shader, or a mesh shader (with an optional task shader), not both",
                                call));
    }
    if (meshPipeline && !device.limits.meshShaders) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no mesh shaders (DeviceLimits::meshShaders)", call));
    }
    // One root data block for the whole pipeline (P44).
    Reflection::DxilInfo info{};
    const char *rootStage = nullptr;
    const auto addStage = [&](Bytecode code, const char *name) {
        if (code.empty()) {
            return;
        }
        const Reflection::DxilInfo stage = reflectStage(code, call);
        if (!stage.hasRootData) {
            return;
        }
        if (rootStage != nullptr && stage.rootDataSize != info.rootDataSize) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("{}: the {} shader declares {} bytes of root data and the {} shader {}; every "
                                    "stage must declare the same root data (P44)",
                                    call, rootStage, info.rootDataSize, name, stage.rootDataSize));
        }
        info.hasRootData = true;
        info.rootDataSize = stage.rootDataSize;
        rootStage = name;
    };
    addStage(desc.task, "task");
    addStage(desc.mesh, "mesh");
    addStage(desc.vertex, "vertex");
    addStage(desc.pixel, "pixel");
    checkRootDataSize(device, info.rootDataSize, call);
    EQ_VALIDATE(Validation::checkGraphicsState(desc.colorFormats.size(), desc.blend.size(), desc.stencil.test,
                                               hasStencil(desc.depthFormat)));
    if (desc.colorFormats.size() > 8) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: at most 8 color formats", call));
    }

    ID3D12RootSignature *rootSignature = createRootSignature(state, info);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature;
    if (!meshPipeline) {
        pso.VS = {desc.vertex.data(), desc.vertex.size()};
    }
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
    pso.BlendState.IndependentBlendEnable = desc.blend.size() > 1 ? TRUE : FALSE;
    for (size_t i = 0; i < desc.blend.size() && i < 8; ++i) {
        const BlendState &b = desc.blend[i];
        pso.BlendState.RenderTarget[i] = {.BlendEnable = b.enable ? TRUE : FALSE,
                                          .LogicOpEnable = FALSE,
                                          .SrcBlend = blendOf(b.srcColor),
                                          .DestBlend = blendOf(b.dstColor),
                                          .BlendOp = blendOpOf(b.colorOp),
                                          .SrcBlendAlpha = alphaBlendOf(b.srcAlpha),
                                          .DestBlendAlpha = alphaBlendOf(b.dstAlpha),
                                          .BlendOpAlpha = blendOpOf(b.alphaOp),
                                          .LogicOp = D3D12_LOGIC_OP_NOOP,
                                          .RenderTargetWriteMask = static_cast<UINT8>(b.writeMask)};
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
    const auto face = [](const StencilFace &f) {
        return D3D12_DEPTH_STENCILOP_DESC{.StencilFailOp = stencilOpOf(f.fail),
                                          .StencilDepthFailOp = stencilOpOf(f.depthFail),
                                          .StencilPassOp = stencilOpOf(f.pass),
                                          .StencilFunc = compareOf(f.compare)};
    };
    pso.DepthStencilState.StencilEnable = desc.stencil.test ? TRUE : FALSE;
    pso.DepthStencilState.StencilReadMask = desc.stencil.readMask;
    pso.DepthStencilState.StencilWriteMask = desc.stencil.writeMask;
    pso.DepthStencilState.FrontFace = face(desc.stencil.front);
    pso.DepthStencilState.BackFace = face(desc.stencil.back);
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
    if (meshPipeline) {
        MeshStream stream;
        stream.rootSignature.value = rootSignature;
        stream.task.value = {desc.task.data(), desc.task.size()};
        stream.mesh.value = {desc.mesh.data(), desc.mesh.size()};
        stream.pixel.value = pso.PS;
        stream.blend.value = pso.BlendState;
        stream.sampleMask.value = pso.SampleMask;
        stream.rasterizer.value = pso.RasterizerState;
        stream.depthStencil.value = pso.DepthStencilState;
        stream.renderTargets.value.NumRenderTargets = pso.NumRenderTargets;
        for (UINT i = 0; i < 8; ++i) {
            stream.renderTargets.value.RTFormats[i] = pso.RTVFormats[i];
        }
        stream.depthFormat.value = pso.DSVFormat;
        stream.sampleDesc.value = pso.SampleDesc;
        const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{.SizeInBytes = sizeof(stream), .pPipelineStateSubobjectStream = &stream};
        checkHr(state.device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState)), "CreatePipelineState (mesh)");
    } else {
        checkHr(state.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState)),
                "CreateGraphicsPipelineState");
    }

    Pipeline out;
    out.rootDataSize = info.hasRootData ? info.rootDataSize : 0;
    out.internal.pipelineState = pipelineState;
    out.internal.rootSignature = rootSignature;
    out.internal.graphics = true;
    out.internal.depthWrite = desc.depth.write;
    out.internal.stencil = desc.stencil.test;
    out.internal.stencilReference = desc.stencil.reference;
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
    forgetCommandSignatures(state, rootSignature);
    if (!pipeline.internal.imported) { // P77: an imported pipeline stays the user's
        release(pso);
        release(rootSignature);
    }
    pipeline = {};
}

// ── Import (P77) ─────────────────────────────────────────────────────────────

Pipeline importPipeline(Device &device, ID3D12PipelineState *pipeline, ID3D12RootSignature *rootSignature,
                        bool graphics, uint32 rootDataSize, D3D_PRIMITIVE_TOPOLOGY topology) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    Pipeline out;
    out.rootDataSize = rootDataSize;
    out.internal.pipelineState = pipeline;
    out.internal.rootSignature = rootSignature;
    out.internal.graphics = graphics;
    out.internal.topology = static_cast<uint32>(topology);
    out.internal.imported = true;
    out.internal.id = EQ_DX_TRACK(&state);
    return out;
}

// ── Work graphs (P50, P62) ───────────────────────────────────────────────────

struct WorkGraphState {
    uint64 backingAddress = 0; // the backing memory the graph last used (P62)
    uint64 backingSize = 0;
    UINT graphIndex = 0;
};

namespace {

constexpr const wchar_t *kProgramName = L"EquineWorkGraph";

void requireWorkGraphs(const Device &device, const char *call) {
    if (!device.limits.workGraphs) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no work graphs (DeviceLimits::workGraphs; Ampere or RDNA3 and newer)",
                                call));
    }
}

ID3D12WorkGraphProperties *workGraphProperties(const WorkGraph &graph) {
    ID3D12WorkGraphProperties *properties = nullptr;
    checkHr(static_cast<ID3D12StateObject *>(graph.internal.stateObject)->QueryInterface(IID_PPV_ARGS(&properties)),
            "ID3D12WorkGraphProperties");
    return properties;
}

ID3D12GraphicsCommandList10 *list10(const CommandList &cmd) {
    ID3D12GraphicsCommandList10 *l = nullptr;
    checkHr(static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list)->QueryInterface(IID_PPV_ARGS(&l)),
            "ID3D12GraphicsCommandList10 (work graphs)");
    return l;
}

} // namespace

WorkGraph createWorkGraph(Device &device, const WorkGraphDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireWorkGraphs(device, "createWorkGraph");
    checkRootDataSize(device, desc.rootDataSize, "createWorkGraph");
    Reflection::DxilInfo info{};
    info.hasRootData = desc.rootDataSize > 0;
    info.rootDataSize = desc.rootDataSize;
    ID3D12RootSignature *rootSignature = createRootSignature(state, info);

    // Every node of the library, one global root signature, one work graph.
    D3D12_DXIL_LIBRARY_DESC library{.DXILLibrary = {desc.library.data(), desc.library.size()}};
    D3D12_GLOBAL_ROOT_SIGNATURE global{.pGlobalRootSignature = rootSignature};
    D3D12_WORK_GRAPH_DESC graphDesc{.ProgramName = kProgramName, .Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES};
    const D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graphDesc},
    };
    const D3D12_STATE_OBJECT_DESC objectDesc{.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE,
                                             .NumSubobjects = static_cast<UINT>(std::size(subobjects)),
                                             .pSubobjects = subobjects};
    ID3D12StateObject *stateObject = nullptr;
    checkHr(state.device->CreateStateObject(&objectDesc, IID_PPV_ARGS(&stateObject)), "CreateStateObject (work graph)");

    WorkGraph graph;
    graph.rootDataSize = desc.rootDataSize;
    graph.internal.stateObject = stateObject;
    graph.internal.rootSignature = rootSignature;
    ID3D12StateObjectProperties1 *objectProperties = nullptr;
    checkHr(stateObject->QueryInterface(IID_PPV_ARGS(&objectProperties)), "ID3D12StateObjectProperties1");
    const D3D12_PROGRAM_IDENTIFIER id = objectProperties->GetProgramIdentifier(kProgramName);
    static_assert(sizeof(id) == sizeof(graph.internal.programId));
    std::memcpy(graph.internal.programId, &id, sizeof(id));
    release(objectProperties);

    auto *s = new WorkGraphState;
    ID3D12WorkGraphProperties *properties = workGraphProperties(graph);
    s->graphIndex = properties->GetWorkGraphIndex(kProgramName);
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
    properties->GetWorkGraphMemoryRequirements(s->graphIndex, &memory);
    release(properties);
    graph.backing = {.minSize = memory.MinSizeInBytes, .maxSize = memory.MaxSizeInBytes};
    graph.internal.state = s;
    graph.internal.id = EQ_DX_TRACK(&state);
    return graph;
}

void destroyWorkGraph(Device &device, WorkGraph &graph) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, graph.internal.id, "work graph");
    EQ_DX_UNTRACK(&state, graph.internal.id);
    auto *stateObject = static_cast<ID3D12StateObject *>(graph.internal.stateObject);
    auto *rootSignature = static_cast<ID3D12RootSignature *>(graph.internal.rootSignature);
    release(stateObject);
    release(rootSignature);
    delete graph.internal.state;
    graph = {};
}

uint32 workGraphEntry(const WorkGraph &graph, std::string_view node, uint32 arrayIndex) {
    const std::wstring name(node.begin(), node.end());
    ID3D12WorkGraphProperties *properties = workGraphProperties(graph);
    const UINT index = properties->GetEntrypointIndex(graph.internal.state->graphIndex, {name.c_str(), arrayIndex});
    release(properties);
    if (index == UINT_MAX) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("workGraphEntry: the graph has no entry node '{}' [{}]", node, arrayIndex));
    }
    return index;
}

void cmdSetWorkGraph(CommandList &cmd, const WorkGraph &graph, const Buffer &backing, uint64 offset, uint64 size,
                     bool forceInitialize) {
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, graph.internal.id, "work graph");
    EQ_DX_CHECK_ALIVE(&state, backing.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkHeapSetBeforePipeline(cmd.internal.heapSet));
    EQ_VALIDATE(Validation::checkRange(offset, size, backing.size, 8, "cmdSetWorkGraph: the backing memory"));
    if (size < graph.backing.minSize || (backing.usage & BufferUsage::eReadWrite) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("cmdSetWorkGraph: the backing memory needs eReadWrite usage and at least {} bytes",
                                graph.backing.minSize));
    }
    // Initialization is inferred (P62): first use, or another range.
    WorkGraphState &s = *graph.internal.state;
    const uint64 address = backing.gpuAddress + offset;
    const bool initialize = forceInitialize || s.backingAddress != address || s.backingSize != size;
    s.backingAddress = address;
    s.backingSize = size;

    ID3D12GraphicsCommandList10 *l = list10(cmd);
    auto *rootSignature = static_cast<ID3D12RootSignature *>(graph.internal.rootSignature);
    l->SetComputeRootSignature(rootSignature);
    cmd.internal.rootSignature[0] = rootSignature;
    cmd.internal.graphics = false;
    cmd.internal.rootDataSize = graph.rootDataSize;
    cmd.internal.pipelineState[0] = nullptr; // SetProgram replaces the pipeline state
    D3D12_SET_PROGRAM_DESC program{.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH};
    std::memcpy(&program.WorkGraph.ProgramIdentifier, graph.internal.programId, sizeof(graph.internal.programId));
    program.WorkGraph.Flags = initialize ? D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE : D3D12_SET_WORK_GRAPH_FLAG_NONE;
    program.WorkGraph.BackingMemory = {.StartAddress = address, .SizeInBytes = size};
    l->SetProgram(&program);
    release(l);
}

void cmdDispatchGraph(CommandList &cmd, std::span<const GraphInput> inputs) {
    if (inputs.empty()) {
        return;
    }
    ID3D12GraphicsCommandList10 *l = list10(cmd);
    D3D12_DISPATCH_GRAPH_DESC desc{};
    std::vector<D3D12_NODE_CPU_INPUT> nodes;
    for (const GraphInput &input : inputs) {
        nodes.push_back({.EntrypointIndex = input.entry,
                         .NumRecords = input.recordCount,
                         .pRecords = const_cast<void *>(input.records),
                         .RecordStrideInBytes = input.recordStride});
    }
    if (nodes.size() == 1) {
        desc.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
        desc.NodeCPUInput = nodes.front();
    } else {
        desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
        desc.MultiNodeCPUInput = {.NumNodeInputs = static_cast<UINT>(nodes.size()),
                                  .pNodeInputs = nodes.data(),
                                  .NodeInputStrideInBytes = sizeof(D3D12_NODE_CPU_INPUT)};
    }
    l->DispatchGraph(&desc); // records are copied into the command list here
    release(l);
}

void cmdDispatchGraphIndirect(CommandList &cmd, uint64 inputAddress, bool multiNode) {
    ID3D12GraphicsCommandList10 *l = list10(cmd);
    D3D12_DISPATCH_GRAPH_DESC desc{};
    if (multiNode) {
        desc.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT;
        desc.MultiNodeGPUInput = inputAddress;
    } else {
        desc.Mode = D3D12_DISPATCH_MODE_NODE_GPU_INPUT;
        desc.NodeGPUInput = inputAddress;
    }
    l->DispatchGraph(&desc);
    release(l);
}

} // namespace RI::D3D12
