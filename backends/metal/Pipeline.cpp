// Pipelines on Metal (P38, P44, P45, P114).
//
// Owns: compute, graphics and mesh pipelines from .mtl.dxil shaders (the DXIL
// container plus an "MTLB" part holding Metal Shader Converter's metallib):
// RI's DXIL reader gives the facts (numthreads, root data, 64-bit atomics,
// declared resources, signatures), the metallib gives the functions of an
// MTL::ComputePipelineState, an MTL::RenderPipelineState (from an
// MTLRenderPipelineDescriptor, or an MTLMeshRenderPipelineDescriptor for mesh
// pipelines, the task shader as Metal's object function) and an
// MTL::DepthStencilState.
//
// Watch out: Metal dispatches and mesh draws name threads per threadgroup,
// so the DXIL's numthreads is kept with the pipeline (P114 (2)). Cull mode,
// winding, fill mode, depth bias and depth clip mode are render encoder state
// on Metal: the pipeline keeps them for cmdSetPipeline (P38 holds: they are
// fixed per pipeline). A pipeline's label can only be set through its
// descriptor, so the creation name goes there; setName after creation only
// renames the tracked object. Geometry, hull and domain shaders are
// unsupported (P116 (1)). In validation builds the pixel shader's inputs are
// matched to the outputs of the vertex or mesh shader by semantic and
// register (P101), from the DXIL signatures as on D3D12, and 64-bit atomics
// are checked against DeviceLimits (P105).
//
// D3D12 counterpart: backends/d3d12/Pipeline.cpp.

#include "DeviceState.hpp"

#include <RI/Reflection/Dxil.hpp>
#include <RI/Validation/Checks.hpp>

#include <dispatch/dispatch.h>

#include <bit>
#include <format>
#include <string>
#include <vector>

namespace RI::Metal {

namespace {

// Reads the shader's DXIL and rejects what RI does not set up yet. In
// validation builds, also what the device lacks (P105).
Reflection::DxilInfo reflectStage([[maybe_unused]] const Device &device, Bytecode bytecode, const char *call,
                                  [[maybe_unused]] const char *stage) {
    const Reflection::DxilReadResult read = Reflection::readDxil(bytecode);
    if (!read.error.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: {} (the Metal backend takes .mtl.dxil: DXIL with the metallib in an MTLB part)",
                                call, read.error));
    }
    EQ_VALIDATE(Validation::checkInt64Atomics(read.info.int64HeapAtomics, read.info.int64SharedAtomics,
                                              device.limits.int64BufferAtomics, device.limits.int64SharedAtomics,
                                              call, stage));
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
                    std::format("{}: the shader's root data is {} bytes; Metal allows {} (P44)", call, size,
                                device.limits.maxRootDataSize));
    }
}

// The metallib of a .mtl.dxil as an MTL::Library (owned).
MTL::Library *newLibrary(DeviceState &state, Bytecode bytecode, const char *call) {
    const std::span<const std::byte> metallib = Reflection::findDxilPart(bytecode, "MTLB");
    if (metallib.empty()) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the shader has no MTLB part (the Metal backend takes .mtl.dxil: DXIL with the "
                                "metallib from Metal Shader Converter in an MTLB part)",
                                call));
    }
    // The default destructor copies the bytes: the caller's bytecode may go
    // away while Metal still holds the dispatch data.
    dispatch_data_t data = dispatch_data_create(metallib.data(), metallib.size(), nullptr,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error *error = nullptr;
    MTL::Library *library = state.device->newLibrary(data, &error);
    dispatch_release(data);
    if (library == nullptr) {
        reportNsError(ErrorCategory::eProgrammerError, std::format("{}: newLibrary (the MTLB part)", call), error);
    }
    return library;
}

// The shader's entry point (owned).
MTL::Function *newMain(MTL::Library *library, const char *call, const char *stage) {
    MTL::Function *function = library->newFunction(nsString("main"));
    if (function == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the {} shader's metallib has no function \"main\" (compile the shader with "
                                "entry point main)",
                                call, stage));
    }
    return function;
}

#if EQUINE_VALIDATION
// The values a signature passes between stages (P101): user values and
// SV_Position, as D3D12 matches them (MSC keeps D3D12's registers). Other
// system values are made by the GPU or rarely passed, and are not checked.
std::vector<Validation::StageValue> stageValues(const std::vector<Reflection::DxilSignatureElement> &elements,
                                                bool slotChecked) {
    std::vector<Validation::StageValue> values;
    for (const Reflection::DxilSignatureElement &e : elements) {
        if (e.systemValue != Reflection::kDxilSystemValueNone && e.systemValue != Reflection::kDxilSystemValuePosition) {
            continue;
        }
        values.push_back({.semantic = Validation::stageSemantic(e.semanticName + std::to_string(e.semanticIndex)),
                          .slot = e.registerIndex,
                          .component = e.mask == 0 ? 0u : static_cast<uint32>(std::countr_zero(e.mask)),
                          .slotChecked = slotChecked});
    }
    return values;
}

// The outputs of the stage before the pixel shader. Per-primitive mesh
// outputs are numbered apart from the pixel shader's registers, so only their
// semantic is matched.
std::vector<Validation::StageValue> stageOutputs(const Reflection::DxilInfo &producer, bool mesh) {
    std::vector<Validation::StageValue> values = stageValues(producer.outputs, true);
    if (mesh) {
        for (Validation::StageValue &value : stageValues(producer.primitiveOutputs, false)) {
            values.push_back(std::move(value));
        }
    }
    return values;
}
#endif

// Geometry and tessellation are not on Metal yet (P116 (1)); Vulkan's messages.
void checkStageSupport(const Device &device, const GraphicsPipelineDesc &desc, const char *call) {
    if (!desc.geometry.empty() && !device.limits.geometryShaders) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no geometry shaders (DeviceLimits::geometryShaders)", call));
    }
    if ((!desc.hull.empty() || !desc.domain.empty() || desc.topology == Topology::ePatchList) &&
        !device.limits.tessellation) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no tessellation (DeviceLimits::tessellation)", call));
    }
}

MTL::BlendFactor blendOf(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::eZero: return MTL::BlendFactorZero;
    case BlendFactor::eOne: return MTL::BlendFactorOne;
    case BlendFactor::eSrcColor: return MTL::BlendFactorSourceColor;
    case BlendFactor::eOneMinusSrcColor: return MTL::BlendFactorOneMinusSourceColor;
    case BlendFactor::eDstColor: return MTL::BlendFactorDestinationColor;
    case BlendFactor::eOneMinusDstColor: return MTL::BlendFactorOneMinusDestinationColor;
    case BlendFactor::eSrcAlpha: return MTL::BlendFactorSourceAlpha;
    case BlendFactor::eOneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
    case BlendFactor::eDstAlpha: return MTL::BlendFactorDestinationAlpha;
    case BlendFactor::eOneMinusDstAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
    }
    return MTL::BlendFactorOne;
}

// Alpha factors naming a color mean the matching alpha factor, as on D3D12
// (which rejects *_COLOR in alpha slots).
MTL::BlendFactor alphaBlendOf(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::eSrcColor: return MTL::BlendFactorSourceAlpha;
    case BlendFactor::eOneMinusSrcColor: return MTL::BlendFactorOneMinusSourceAlpha;
    case BlendFactor::eDstColor: return MTL::BlendFactorDestinationAlpha;
    case BlendFactor::eOneMinusDstColor: return MTL::BlendFactorOneMinusDestinationAlpha;
    default: return blendOf(factor);
    }
}

MTL::BlendOperation blendOpOf(BlendOp op) {
    switch (op) {
    case BlendOp::eAdd: return MTL::BlendOperationAdd;
    case BlendOp::eSubtract: return MTL::BlendOperationSubtract;
    case BlendOp::eReverseSubtract: return MTL::BlendOperationReverseSubtract;
    case BlendOp::eMin: return MTL::BlendOperationMin;
    case BlendOp::eMax: return MTL::BlendOperationMax;
    }
    return MTL::BlendOperationAdd;
}

// RI numbers channels R = bit 0 (D3D12's order); Metal R = bit 3.
MTL::ColorWriteMask writeMaskOf(ColorMask mask) {
    NS::UInteger out = MTL::ColorWriteMaskNone;
    if ((mask & ColorMask::eR) != ColorMask::eNone) {
        out |= MTL::ColorWriteMaskRed;
    }
    if ((mask & ColorMask::eG) != ColorMask::eNone) {
        out |= MTL::ColorWriteMaskGreen;
    }
    if ((mask & ColorMask::eB) != ColorMask::eNone) {
        out |= MTL::ColorWriteMaskBlue;
    }
    if ((mask & ColorMask::eA) != ColorMask::eNone) {
        out |= MTL::ColorWriteMaskAlpha;
    }
    return static_cast<MTL::ColorWriteMask>(out);
}

// RI's CompareOp and StencilOp follow Metal's order (as samplers rely on).
MTL::CompareFunction compareOf(CompareOp op) { return static_cast<MTL::CompareFunction>(op); }
MTL::StencilOperation stencilOpOf(StencilOp op) { return static_cast<MTL::StencilOperation>(op); }

MTL::PrimitiveTopologyClass topologyClassOf(Topology topology) {
    switch (topology) {
    case Topology::eLineList:
    case Topology::eLineStrip: return MTL::PrimitiveTopologyClassLine;
    case Topology::ePointList: return MTL::PrimitiveTopologyClassPoint;
    default: return MTL::PrimitiveTopologyClassTriangle;
    }
}

MTL::PrimitiveType primitiveTypeOf(Topology topology) {
    switch (topology) {
    case Topology::eTriangleStrip: return MTL::PrimitiveTypeTriangleStrip;
    case Topology::eLineList: return MTL::PrimitiveTypeLine;
    case Topology::eLineStrip: return MTL::PrimitiveTypeLineStrip;
    case Topology::ePointList: return MTL::PrimitiveTypePoint;
    default: return MTL::PrimitiveTypeTriangle;
    }
}

MTL::PixelFormat attachmentFormatOf(Format format, const char *call) {
    const MTL::PixelFormat pixelFormat = pixelFormatOf(format);
    if (pixelFormat == MTL::PixelFormatInvalid) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: format {} has no Metal pixel format (formatSupported)", call, formatName(format)));
    }
    return pixelFormat;
}

// Attachment formats, blending and samples: the same on both descriptor kinds.
template <typename Descriptor>
void describeTargets(Descriptor *native, const GraphicsPipelineDesc &desc, const char *call) {
    for (size_t i = 0; i < desc.colorFormats.size(); ++i) {
        MTL::RenderPipelineColorAttachmentDescriptor *target = native->colorAttachments()->object(i);
        target->setPixelFormat(attachmentFormatOf(desc.colorFormats[i], call));
        // No blend states: every target written as is (GraphicsPipelineDesc::blend).
        if (i >= desc.blend.size()) {
            continue;
        }
        const BlendState &b = desc.blend[i];
        target->setBlendingEnabled(b.enable);
        target->setSourceRGBBlendFactor(blendOf(b.srcColor));
        target->setDestinationRGBBlendFactor(blendOf(b.dstColor));
        target->setRgbBlendOperation(blendOpOf(b.colorOp));
        target->setSourceAlphaBlendFactor(alphaBlendOf(b.srcAlpha));
        target->setDestinationAlphaBlendFactor(alphaBlendOf(b.dstAlpha));
        target->setAlphaBlendOperation(blendOpOf(b.alphaOp));
        target->setWriteMask(writeMaskOf(b.writeMask));
    }
    if (isDepthFormat(desc.depthFormat)) {
        native->setDepthAttachmentPixelFormat(attachmentFormatOf(desc.depthFormat, call));
        if (hasStencil(desc.depthFormat)) {
            native->setStencilAttachmentPixelFormat(attachmentFormatOf(desc.depthFormat, call));
        }
    }
    native->setRasterSampleCount(desc.samples);
}

// The depth and stencil tests (owned). Writing depth needs the test on in
// D3D12; Metal writes with any compare, so a write without a test compares
// with eAlways, as RI's D3D12 backend does.
MTL::DepthStencilState *newDepthStencilState(DeviceState &state, const GraphicsPipelineDesc &desc) {
    MTL::DepthStencilDescriptor *native = MTL::DepthStencilDescriptor::alloc()->init();
    native->setDepthCompareFunction(desc.depth.test ? compareOf(desc.depth.compare) : MTL::CompareFunctionAlways);
    native->setDepthWriteEnabled(desc.depth.write);
    MTL::StencilDescriptor *faces[2] = {nullptr, nullptr};
    if (desc.stencil.test) {
        const StencilFace *sides[2] = {&desc.stencil.front, &desc.stencil.back};
        for (int i = 0; i < 2; ++i) {
            faces[i] = MTL::StencilDescriptor::alloc()->init();
            faces[i]->setStencilCompareFunction(compareOf(sides[i]->compare));
            faces[i]->setStencilFailureOperation(stencilOpOf(sides[i]->fail));
            faces[i]->setDepthFailureOperation(stencilOpOf(sides[i]->depthFail));
            faces[i]->setDepthStencilPassOperation(stencilOpOf(sides[i]->pass));
            faces[i]->setReadMask(desc.stencil.readMask);
            faces[i]->setWriteMask(desc.stencil.writeMask);
        }
        native->setFrontFaceStencil(faces[0]);
        native->setBackFaceStencil(faces[1]);
    }
    if (!desc.name.empty()) {
        native->setLabel(nsString(desc.name));
    }
    MTL::DepthStencilState *depthStencil = state.device->newDepthStencilState(native);
    release(faces[0]);
    release(faces[1]);
    release(native);
    if (depthStencil == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "createGraphicsPipeline: newDepthStencilState failed");
    }
    return depthStencil;
}

} // namespace

Pipeline createComputePipeline(Device &device, const ComputePipelineDesc &desc) {
    DeviceState &state = *device.internal.state;
    const char *call = "createComputePipeline";
    const Reflection::DxilInfo info = reflectStage(device, desc.shader, call, "compute");
    checkRootDataSize(device, info.rootDataSize, call);

    // Error objects and strings below are autoreleased.
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL::Library *library = newLibrary(state, desc.shader, call);
    MTL::Function *function = library->newFunction(nsString("main"));
    if (function == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the metallib has no function \"main\" (compile the shader with entry point main)",
                                call));
    }
    MTL::ComputePipelineDescriptor *pipelineDesc = MTL::ComputePipelineDescriptor::alloc()->init();
    pipelineDesc->setComputeFunction(function);
    if (!desc.name.empty()) {
        pipelineDesc->setLabel(nsString(desc.name));
    }
    NS::Error *error = nullptr;
    MTL::ComputePipelineState *pipelineState =
        state.device->newComputePipelineState(pipelineDesc, MTL::PipelineOptionNone, nullptr, &error);
    release(pipelineDesc);
    release(function);
    release(library);
    if (pipelineState == nullptr) {
        // reportNsError aborts (P57), so nothing is cleaned up first.
        reportNsError(ErrorCategory::eProgrammerError, std::format("{}: newComputePipelineState", call), error);
    }
    pool->release();

    Pipeline out;
    out.rootDataSize = info.hasRootData ? info.rootDataSize : 0;
    out.internal.pipelineState = pipelineState;
    for (int i = 0; i < 3; ++i) {
        out.internal.threadGroupSize[i] = info.threadGroupSize[i] != 0 ? info.threadGroupSize[i] : 1;
    }
    out.internal.id = EQ_MTL_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(state, out, desc.name);
    }
    return out;
}

// Graphics pipelines (P38, P39, P102): complete state up front, no vertex
// input (vertices are pulled by the shader).
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
    checkStageSupport(device, desc, call);
    EQ_VALIDATE(Validation::checkGraphicsStages(meshPipeline, !desc.hull.empty(), !desc.domain.empty(),
                                                !desc.geometry.empty(), desc.topology == Topology::ePatchList,
                                                desc.patchControlPoints));
    // One root data block for the whole pipeline (P44).
    Reflection::DxilInfo root{};
    const char *rootStage = nullptr;
    const auto addStage = [&](Bytecode code, const char *name) -> Reflection::DxilInfo {
        if (code.empty()) {
            return {};
        }
        Reflection::DxilInfo stage = reflectStage(device, code, call, name);
        if (!stage.hasRootData) {
            return stage;
        }
        if (rootStage != nullptr && stage.rootDataSize != root.rootDataSize) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("{}: the {} shader declares {} bytes of root data and the {} shader {}; every "
                                    "stage must declare the same root data (P44)",
                                    call, rootStage, root.rootDataSize, name, stage.rootDataSize));
        }
        root.hasRootData = true;
        root.rootDataSize = stage.rootDataSize;
        rootStage = name;
        return stage;
    };
    const Reflection::DxilInfo task = addStage(desc.task, "task");
    const Reflection::DxilInfo mesh = addStage(desc.mesh, "mesh");
    [[maybe_unused]] const Reflection::DxilInfo vertex = addStage(desc.vertex, "vertex");
    [[maybe_unused]] const Reflection::DxilInfo pixel = addStage(desc.pixel, "pixel");
    if (!desc.pixel.empty()) {
        EQ_VALIDATE(Validation::checkStageLink(stageOutputs(meshPipeline ? mesh : vertex, meshPipeline),
                                               stageValues(pixel.inputs, true), meshPipeline ? "mesh" : "vertex",
                                               "register"));
    }
    checkRootDataSize(device, root.rootDataSize, call);
    EQ_VALIDATE(Validation::checkGraphicsState(desc.colorFormats.size(), desc.blend.size(), desc.stencil.test,
                                               hasStencil(desc.depthFormat)));
    if (desc.colorFormats.size() > 8) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: at most 8 color formats", call));
    }

    // Libraries, functions, descriptors and error objects are released or
    // autoreleased below; the pipeline and depth-stencil states are RI's.
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    struct Stage {
        MTL::Library *library = nullptr;
        MTL::Function *function = nullptr;
    };
    const auto load = [&](Bytecode code, const char *name) -> Stage {
        if (code.empty()) {
            return {};
        }
        Stage stage{.library = newLibrary(state, code, call)};
        stage.function = newMain(stage.library, call, name);
        return stage;
    };
    Stage stages[] = {load(desc.task, "task"), load(desc.mesh, "mesh"), load(desc.vertex, "vertex"),
                      load(desc.pixel, "pixel")};
    const Stage &taskStage = stages[0], &meshStage = stages[1], &vertexStage = stages[2], &pixelStage = stages[3];
    NS::Error *error = nullptr;
    MTL::RenderPipelineState *pipelineState = nullptr;
    if (meshPipeline) {
        MTL::MeshRenderPipelineDescriptor *native = MTL::MeshRenderPipelineDescriptor::alloc()->init();
        native->setObjectFunction(taskStage.function);
        native->setMeshFunction(meshStage.function);
        native->setFragmentFunction(pixelStage.function);
        describeTargets(native, desc, call);
        if (!desc.name.empty()) {
            native->setLabel(nsString(desc.name));
        }
        pipelineState = state.device->newRenderPipelineState(native, MTL::PipelineOptionNone, nullptr, &error);
        release(native);
    } else {
        MTL::RenderPipelineDescriptor *native = MTL::RenderPipelineDescriptor::alloc()->init();
        native->setVertexFunction(vertexStage.function);
        native->setFragmentFunction(pixelStage.function);
        native->setInputPrimitiveTopology(topologyClassOf(desc.topology));
        describeTargets(native, desc, call);
        if (!desc.name.empty()) {
            native->setLabel(nsString(desc.name));
        }
        pipelineState = state.device->newRenderPipelineState(native, MTL::PipelineOptionNone, nullptr, &error);
        release(native);
    }
    for (Stage &stage : stages) {
        release(stage.function);
        release(stage.library);
    }
    if (pipelineState == nullptr) {
        // reportNsError aborts (P57), so nothing is cleaned up first.
        reportNsError(ErrorCategory::eProgrammerError, std::format("{}: newRenderPipelineState", call), error);
    }
    MTL::DepthStencilState *depthStencil = newDepthStencilState(state, desc);
    pool->release();

    Pipeline out;
    out.rootDataSize = root.hasRootData ? root.rootDataSize : 0;
    auto &in = out.internal;
    in.pipelineState = pipelineState;
    in.depthStencilState = depthStencil;
    in.graphics = true;
    in.mesh = meshPipeline;
    in.task = !desc.task.empty();
    for (int i = 0; i < 3; ++i) {
        in.threadGroupSize[i] = mesh.threadGroupSize[i] != 0 ? mesh.threadGroupSize[i] : 1;
        in.taskThreadGroupSize[i] = task.threadGroupSize[i] != 0 ? task.threadGroupSize[i] : 1;
    }
    in.depthWrite = desc.depth.write;
    in.stencilReference = desc.stencil.reference;
    in.primitiveType = static_cast<uint8>(primitiveTypeOf(desc.topology));
    in.cullMode = static_cast<uint8>(desc.cull == CullMode::eFront  ? MTL::CullModeFront
                                     : desc.cull == CullMode::eBack ? MTL::CullModeBack
                                                                    : MTL::CullModeNone);
    // Both count windings in framebuffer space with y down (MSC keeps D3D12's
    // clip space), so D3D12's default, clockwise front faces, carries over.
    in.winding = static_cast<uint8>(desc.frontCounterClockwise ? MTL::WindingCounterClockwise : MTL::WindingClockwise);
    in.fillMode = static_cast<uint8>(desc.fill == FillMode::eWireframe ? MTL::TriangleFillModeLines
                                                                       : MTL::TriangleFillModeFill);
    in.depthClipMode = static_cast<uint8>(desc.depthClamp ? MTL::DepthClipModeClamp : MTL::DepthClipModeClip);
    // Metal's constant bias counts the format's smallest depth steps, as D3D12's integer does.
    in.depthBias[0] = static_cast<float>(desc.depthBias.constant);
    in.depthBias[1] = desc.depthBias.slope;
    in.depthBias[2] = desc.depthBias.clamp;
    in.id = EQ_MTL_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(state, out, desc.name);
    }
    return out;
}

void destroyPipeline(Device &device, Pipeline &pipeline) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, pipeline.internal.id, "pipeline");
    EQ_MTL_UNTRACK(&state, pipeline.internal.id);
    if (!pipeline.internal.imported) { // P77: an imported pipeline stays the user's
        auto *object = static_cast<NS::Object *>(pipeline.internal.pipelineState);
        auto *depthStencil = static_cast<NS::Object *>(pipeline.internal.depthStencilState);
        release(object);
        release(depthStencil);
    }
    pipeline = {};
}

} // namespace RI::Metal
