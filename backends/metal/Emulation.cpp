// Geometry and tessellation on Metal through MSC's emulation (P116 (1) and
// P114, amended 2026-11-09).
//
// Owns: DeviceState::emulation (the buffer of the empty vertex-buffer table
// and MSC's tessellator tables), emulation pipelines (MSC's runtime helpers
// IRRuntimeNewGeometryEmulationPipeline and
// IRRuntimeNewGeometryTessellationEmulationPipeline), their draws (MSC's
// calculation functions, RI's binding), and the empty fragment function of
// depth-only emulated pipelines.
//
// How: MSC turns the vertex shader into an object function (it runs the
// vertex shader, with a stage-in function linked in, the MTLS part), the
// geometry or domain shader into a mesh function, and the hull shader into
// object-stage work; the pipeline is a mesh pipeline, drawn with
// drawMeshThreadgroups. A draw computes the threadgroup counts on the CPU from
// the draw and the pipeline's reflection numbers (the MTLR parts, written at
// build time by equine_metal_pack), writes the draw arguments and MSC's draw
// info to command memory (P115) and points argument-table indices 4 and 5 at
// them. Root data is bound at index 2 and, for hull and domain shaders, 3
// (cmdSetRootData sets both).
//
// Watch out: indirect draws of these pipelines are unsupported (Indirect.cpp
// reports them): the helpers compute threadgroup counts on the CPU. RI has
// no vertex inputs, so the vertex-buffer table at index 6 stays empty and the
// stage-in function was made for an empty layout. MSC's helpers build the
// pipeline with the classic MTLDevice call; Metal 4 encoders take it.
//
// Vulkan and D3D12 have native stages; nothing corresponds there.

#include "DeviceState.hpp"

#include <RI/Reflection/Dxil.hpp>

#define IR_RUNTIME_METALCPP
#define IR_RUNTIME_METAL4
#define IR_PRIVATE_IMPLEMENTATION
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>
// After the runtime header, whose types it uses.
#include <metal_irconverter_runtime/ir_tessellator_tables.h>

#include <array>
#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

// The vertex-buffer table, then the tables at a 256-byte boundary.
constexpr uint64 kTessellatorTablesOffset = (sizeof(IRRuntimeVertexBuffers) + 255) & ~uint64{255};

// The MTLR part: equine_metal_pack's kReflectionFields, after a version.
enum ReflectionField : uint8 {
    kVersion,
    kVertexOutputSize,
    kGsMaxInputPrimitives,
    kGsInstanceCount,
    kHsMaxPatches,
    kHsInputControlPoints,
    kHsThreadsPerPatch,
    kHsMaxTessellationFactor,
    kHsOutputPrimitive,
    kDsMaxInputPrimitives,
    kReflectionFieldCount,
};
using ReflectionNumbers = std::array<uint32, kReflectionFieldCount>;

ReflectionNumbers readReflection(Bytecode bytecode, const char *call, const char *stage) {
    const std::span<const std::byte> part = Reflection::findDxilPart(bytecode, "MTLR");
    ReflectionNumbers numbers{};
    if (part.size() != sizeof(numbers)) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the {} shader has no MTLR part of {} bytes; geometry and tessellation need "
                                "shaders converted by equine_add_shaders since 2026-11-09 (P114)",
                                call, stage, sizeof(numbers)));
    }
    std::memcpy(numbers.data(), part.data(), sizeof(numbers));
    if (numbers[kVersion] != 1) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the {} shader's MTLR part has version {}; this RI reads version 1", call, stage,
                                numbers[kVersion]));
    }
    return numbers;
}

IRRuntimePrimitiveType primitiveOf(Topology topology) {
    switch (topology) {
    case Topology::ePointList: return IRRuntimePrimitiveTypePoint;
    case Topology::eLineList: return IRRuntimePrimitiveTypeLine;
    case Topology::eLineStrip: return IRRuntimePrimitiveTypeLineStrip;
    case Topology::eTriangleStrip: return IRRuntimePrimitiveTypeTriangleStrip;
    case Topology::eTriangleList:
    case Topology::ePatchList: return IRRuntimePrimitiveTypeTriangle; // every patch list, in MSC's enum
    }
    return IRRuntimePrimitiveTypeTriangle;
}

// MSC's mesh function for tessellation without a geometry shader.
const char *passthroughOf(uint32 tessellatorOutput) {
    switch (tessellatorOutput) {
    case IRRuntimeTessellatorOutputPoint: return kIRPointPassthroughGeometryShader;
    case IRRuntimeTessellatorOutputLine: return kIRLinePassthroughGeometryShader;
    default: return kIRTrianglePassthroughGeometryShader;
    }
}

// RI's empty fragment function (owned by DeviceState::builtins).
MTL::Library *emptyFragmentLibrary(DeviceState &state) {
    const std::lock_guard lock(state.builtinsMutex);
    if (state.builtins.emptyFragment == nullptr) {
        NS::Error *error = nullptr;
        state.builtins.emptyFragment =
            state.device->newLibrary(nsString("fragment void equine_empty_fragment() {}"), nullptr, &error);
        if (state.builtins.emptyFragment == nullptr) {
            reportNsError(ErrorCategory::eProgrammerError, "createGraphicsPipeline: RI's empty fragment function",
                          error);
        }
    }
    return state.builtins.emptyFragment;
}

IRRuntimeGeometryPipelineConfig geometryConfigOf(const Pipeline &pipeline) {
    const auto &c = pipeline.internal.emulationConfig;
    return {.gsVertexSizeInBytes = c[0], .gsMaxInputPrimitivesPerMeshThreadgroup = c[1]};
}

IRRuntimeTessellationPipelineConfig tessellationConfigOf(const Pipeline &pipeline) {
    const auto &in = pipeline.internal;
    const auto &c = in.emulationConfig;
    return {.outputPrimitiveType = static_cast<IRRuntimeTessellatorOutputPrimitive>(in.tessellatorOutput),
            .vsOutputSizeInBytes = c[0],
            .gsMaxInputPrimitivesPerMeshThreadgroup = c[1],
            .hsMaxPatchesPerObjectThreadgroup = c[3],
            .hsInputControlPointCount = c[4],
            .hsMaxObjectThreadsPerThreadgroup = c[5],
            .hsMaxTessellationFactor = in.maxTessellationFactor,
            .gsInstanceCount = c[2]};
}

} // namespace

MTL::Buffer *newEmulationBuffer(DeviceState &state) {
    const uint64 size = kTessellatorTablesOffset + IRRuntimeTessellatorTablesSize();
    MTL::Buffer *buffer = state.device->newBuffer(size, MTL::ResourceStorageModeShared);
    if (buffer == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "newBuffer failed (geometry and tessellation tables)");
    }
    std::memset(buffer->contents(), 0, kTessellatorTablesOffset);
    // The loader writes at a buffer's start: load into a staging buffer, then
    // copy behind the vertex-buffer table.
    MTL::Buffer *staging = state.device->newBuffer(IRRuntimeTessellatorTablesSize(), MTL::ResourceStorageModeShared);
    if (staging == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "newBuffer failed (tessellator tables)");
    }
    IRRuntimeLoadTessellatorTables(staging);
    std::memcpy(static_cast<std::byte *>(buffer->contents()) + kTessellatorTablesOffset, staging->contents(),
                IRRuntimeTessellatorTablesSize());
    staging->release();
    return buffer;
}

MTL::RenderPipelineState *newEmulationPipeline(DeviceState &state, const GraphicsPipelineDesc &desc,
                                               MTL::MeshRenderPipelineDescriptor *base, Pipeline &out,
                                               const char *call) {
    const bool tessellation = !desc.hull.empty();
    const bool geometry = !desc.geometry.empty();
    const ReflectionNumbers vertex = readReflection(desc.vertex, call, "vertex");
    const ReflectionNumbers gs = geometry ? readReflection(desc.geometry, call, "geometry") : ReflectionNumbers{};
    const ReflectionNumbers hs = tessellation ? readReflection(desc.hull, call, "hull") : ReflectionNumbers{};
    const ReflectionNumbers ds = tessellation ? readReflection(desc.domain, call, "domain") : ReflectionNumbers{};

    auto &in = out.internal;
    in.emulation = tessellation ? 2 : 1;
    in.emulationTopology = static_cast<uint8>(primitiveOf(desc.topology));
    in.tessellatorOutput = static_cast<uint8>(hs[kHsOutputPrimitive]);
    in.emulationConfig[0] = vertex[kVertexOutputSize];
    in.emulationConfig[1] = geometry ? gs[kGsMaxInputPrimitives] : ds[kDsMaxInputPrimitives];
    in.emulationConfig[2] = geometry ? gs[kGsInstanceCount] : 1;
    in.emulationConfig[3] = hs[kHsMaxPatches];
    in.emulationConfig[4] = hs[kHsInputControlPoints];
    in.emulationConfig[5] = hs[kHsThreadsPerPatch];
    std::memcpy(&in.maxTessellationFactor, &hs[kHsMaxTessellationFactor], sizeof(float));

    MTL::Library *stageIn = newShaderLibrary(state, desc.vertex, call, "MTLS");
    MTL::Library *vertexLibrary = newShaderLibrary(state, desc.vertex, call);
    MTL::Library *geometryLibrary = geometry ? newShaderLibrary(state, desc.geometry, call) : nullptr;
    MTL::Library *hullLibrary = tessellation ? newShaderLibrary(state, desc.hull, call) : nullptr;
    MTL::Library *domainLibrary = tessellation ? newShaderLibrary(state, desc.domain, call) : nullptr;
    MTL::Library *pixelLibrary = desc.pixel.empty() ? nullptr : newShaderLibrary(state, desc.pixel, call);
    const char *fragmentName = pixelLibrary != nullptr ? "main" : "equine_empty_fragment";

    NS::Error *error = nullptr;
    MTL::RenderPipelineState *pipeline = nullptr;
    if (tessellation) {
        const IRGeometryTessellationEmulationPipelineDescriptor native{
            .stageInLibrary = stageIn,
            .vertexLibrary = vertexLibrary,
            .vertexFunctionName = "main",
            .hullLibrary = hullLibrary,
            .hullFunctionName = "main",
            .domainLibrary = domainLibrary,
            .domainFunctionName = "main",
            .geometryLibrary = geometryLibrary,
            .geometryFunctionName = geometry ? "main" : passthroughOf(in.tessellatorOutput),
            .fragmentLibrary = pixelLibrary != nullptr ? pixelLibrary : emptyFragmentLibrary(state),
            .fragmentFunctionName = fragmentName,
            .basePipelineDescriptor = base,
            .pipelineConfig = tessellationConfigOf(out),
        };
        pipeline = IRRuntimeNewGeometryTessellationEmulationPipeline(state.device, &native, &error);
    } else {
        const IRGeometryEmulationPipelineDescriptor native{
            .stageInLibrary = stageIn,
            .vertexLibrary = vertexLibrary,
            .vertexFunctionName = "main",
            .geometryLibrary = geometryLibrary,
            .geometryFunctionName = "main",
            .fragmentLibrary = pixelLibrary != nullptr ? pixelLibrary : emptyFragmentLibrary(state),
            .fragmentFunctionName = fragmentName,
            .basePipelineDescriptor = base,
            .pipelineConfig = geometryConfigOf(out),
        };
        pipeline = IRRuntimeNewGeometryEmulationPipeline(state.device, &native, &error);
    }
    for (MTL::Library *library : {stageIn, vertexLibrary, geometryLibrary, hullLibrary, domainLibrary, pixelLibrary}) {
        if (library != nullptr) {
            library->release();
        }
    }
    if (pipeline == nullptr) {
        reportNsError(ErrorCategory::eProgrammerError,
                      std::format("{}: MSC's {} emulation pipeline", call, tessellation ? "tessellation" : "geometry"),
                      error);
    }
    return pipeline;
}

// MSC's IRRuntime4Draw*Emulation helpers, with RI's binding: they place the
// draw info 20 bytes into their scratch range, misaligned for its 8-byte
// fields (UBSan reports their store) and for Metal's buffer addresses, so RI
// writes the arguments and the draw info to separate 16-byte-aligned ranges
// of command memory (P115) and encodes the same draw.
void drawEmulated(DeviceState &state, CommandListState &cmd, bool indexed, uint32 count, uint32 instanceCount,
                  uint32 first, int32 vertexOffset, uint32 firstInstance) {
    const Pipeline &pipeline = cmd.graphics;
    const auto topology = static_cast<IRRuntimePrimitiveType>(pipeline.internal.emulationTopology);
    const bool tessellation = pipeline.internal.emulation == 2;
    const MTL::IndexType indexType = !indexed                                 ? static_cast<MTL::IndexType>(-1)
                                     : cmd.indexFormat == IndexFormat::eUint32 ? MTL::IndexTypeUInt32
                                                                               : MTL::IndexTypeUInt16;
    IRRuntimeDrawInfo info{};
    uint32 objectThreads = 0, meshThreads = 0;
    if (tessellation) {
        const IRRuntimeTessellationPipelineConfig c = tessellationConfigOf(pipeline);
        info = IRRuntimeCalculateDrawInfoForGSTSEmulation(topology, indexType, c.outputPrimitiveType,
                                                          c.gsMaxInputPrimitivesPerMeshThreadgroup,
                                                          c.hsMaxPatchesPerObjectThreadgroup, c.hsInputControlPointCount,
                                                          c.hsMaxObjectThreadsPerThreadgroup, c.gsInstanceCount);
        IRRuntimeCalculateThreadgroupSizeForTessellationAndGeometry(c.hsMaxPatchesPerObjectThreadgroup,
                                                                    c.hsMaxObjectThreadsPerThreadgroup,
                                                                    c.gsMaxInputPrimitivesPerMeshThreadgroup,
                                                                    &objectThreads, &meshThreads);
    } else {
        const IRRuntimeGeometryPipelineConfig c = geometryConfigOf(pipeline);
        // The last argument is the geometry shader's [instance(n)] count (the
        // draw info's gsInstanceCount), not the draw's instance count, which
        // MSC's own IRRuntime…GeometryEmulation helpers pass there: with them,
        // a shader of [instance(2)] drawn once runs only its instance 0.
        info = IRRuntimeCalculateDrawInfoForGSEmulation(topology, indexType, c.gsVertexSizeInBytes,
                                                        c.gsMaxInputPrimitivesPerMeshThreadgroup,
                                                        pipeline.internal.emulationConfig[2]);
        IRRuntimeCalculateThreadgroupSizeForGeometry(topology, c.gsMaxInputPrimitivesPerMeshThreadgroup,
                                                     info.objectThreadgroupVertexStride, &objectThreads, &meshThreads);
    }
    IRRuntimeDrawParams params{};
    if (indexed) {
        info.indexBuffer = cmd.indexAddress;
        params.drawIndexed = {.indexCountPerInstance = count,
                              .instanceCount = instanceCount,
                              .startIndexLocation = first,
                              .baseVertexLocation = vertexOffset,
                              .startInstanceLocation = firstInstance};
    } else {
        info.indexType = kIRNonIndexedDraw;
        params.draw = {.vertexCountPerInstance = count,
                       .instanceCount = instanceCount,
                       .startVertexLocation = first,
                       .startInstanceLocation = firstInstance};
    }
    const MTL::Size groups =
        IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation(count, info.objectThreadgroupVertexStride,
                                                                           topology, instanceCount);

    const CommandMemoryAllocation paramsMemory = allocateCommandMemory(state, cmd, sizeof(params), 16);
    std::memcpy(paramsMemory.cpu, &params, sizeof(params));
    const CommandMemoryAllocation infoMemory = allocateCommandMemory(state, cmd, sizeof(info), 16);
    std::memcpy(infoMemory.cpu, &info, sizeof(info));
    cmd.argumentTable->setAddress(paramsMemory.gpuAddress, kDrawArgumentsIndex);
    cmd.argumentTable->setAddress(infoMemory.gpuAddress, kUniformsIndex);
    cmd.argumentTable->setAddress(state.emulation->gpuAddress(), kVertexBuffersIndex);
    if (tessellation) {
        cmd.argumentTable->setAddress(state.emulation->gpuAddress() + kTessellatorTablesOffset, kTessellatorTablesIndex);
        // The helpers' object threadgroup memory for the hull stage.
        cmd.render->setObjectThreadgroupMemoryLength(15360, 0);
    }
    cmd.render->drawMeshThreadgroups(groups, MTL::Size(objectThreads, 1, 1), MTL::Size(meshThreads, 1, 1));
}

} // namespace RI::Metal
