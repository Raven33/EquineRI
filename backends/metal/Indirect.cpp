// Indirect execution on Metal (P48, proposal §7).
//
// Owns: cmdExecuteIndirect, cmdExecuteIndirectWithRootData,
// indirectScratchRequirements (always size 0), and RI's built-in MSL
// (builtinPipelines, released by releaseBuiltins): the count copy, the
// threadgroup counts of indirect draws of emulated geometry and tessellation
// pipelines (emulationGroups, for Emulation.cpp), and the instance repack of
// cmdBuildTlas (RayTracing.cpp).
//
// How: Metal's indirect layouts equal RI's (MTLDrawPrimitivesIndirectArguments,
// MTLDrawIndexedPrimitivesIndirectArguments, MTLDispatchThreadgroupsIndirectArguments),
// so each record is one indirect draw or dispatch reading the record itself.
// MSC's shaders read the draw arguments at kDrawArgumentsIndex: RI points it
// at the record's arguments. Level 2 records start with the root data, MSC's
// top-level argument buffer: RI points kRootDataIndex at the record. No copy
// and no GPU work, so indirectScratchRequirements is 0. Pipelines with
// geometry or tessellation shaders (MSC's emulation) draw each record with
// drawMeshThreadgroups from counts RI's built-in kernel computes first
// (drawEmulatedIndirect); their draw arguments are read as they are too.
//
// A GPU-written count (Q4, P115): Metal has no draw count. RI copies the
// maxCount records into command memory and zeroes, in records at or beyond
// the count, the instance count (draws) or the group count x (dispatches,
// mesh draws), then runs maxCount indirect commands on the copy. The copy is
// RI's built-in MSL (kCountSource, compiled once per device at first use):
// a compute kernel outside rendering sections, and inside one a vertex
// function with rasterization off, since a render encoder cannot dispatch
// compute; an encoder barrier orders it before the indirect reads.
//
// Watch out: the copy overwrites argument-table index 0 (the resource heap)
// and the render pipeline; RI sets them back. An indirect draw counts as a
// draw for cmdWriteTimestamp (sectionDrawn) even if its records are empty.
//
// D3D12 counterpart: backends/d3d12/Indirect.cpp (ExecuteIndirect).

#include "DeviceState.hpp"

#include <RI/Validation/Checks.hpp>

#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

constexpr const char *kCountSource = R"(
#include <metal_stdlib>
using namespace metal;

struct CountParams {
    device const uint *src;
    device uint *dst;
    device const uint *count;
    uint total;     // words to copy: maxCount * words
    uint words;     // words per record
    uint zeroWord;  // the word zeroed in records at or beyond the count
    uint maxCount;
};

static void copyWord(constant CountParams &p, uint v) {
    if (v >= p.total) {
        return;
    }
    const uint record = v / p.words;
    const uint count = min(*p.count, p.maxCount);
    p.dst[v] = (record >= count && v % p.words == p.zeroWord) ? 0u : p.src[v];
}

vertex void equine_count_vertex(constant CountParams &p [[buffer(0)]], uint v [[vertex_id]]) {
    copyWord(p, v);
}

kernel void equine_count_kernel(constant CountParams &p [[buffer(0)]], uint v [[thread_position_in_grid]]) {
    copyWord(p, v);
}

// Emulation.cpp: indirect draws of geometry and tessellation pipelines. For
// record v, MSC's object threadgroup counts from its draw arguments (word 0
// the vertex or index count, word 1 the instance count), as
// IRRuntimeCalculateObjectTgCountForTessellationAndGeometryEmulation does on
// the CPU, without its wrap-around for a strip shorter than one primitive.
struct GroupsParams {
    device const uint *records;
    device uint *groups;
    uint words;      // words per record
    uint argsWord;   // the first word of the draw arguments (after the root data)
    uint maxCount;
    uint vertexStride;
    uint overlap;
};

vertex void equine_emulation_groups(constant GroupsParams &p [[buffer(0)]], uint v [[vertex_id]]) {
    if (v >= p.maxCount) {
        return;
    }
    device const uint *args = p.records + v * p.words + p.argsWord;
    const uint count = args[0];
    p.groups[v * 3 + 0] = count > p.overlap ? (count - p.overlap + p.vertexStride - 1) / p.vertexStride : 0u;
    p.groups[v * 3 + 1] = args[1];
    p.groups[v * 3 + 2] = 1u;
}

// RayTracing.cpp: RI's 64-byte instances (D3D12's) as Metal's 72-byte
// MTLIndirectAccelerationStructureInstanceDescriptor: the row-major 3x4
// transform as is (the build sets MatrixLayoutRowMajor), the flags as the
// options (same bits), the mask, the instance ID as the user ID, and the
// bottom level's resource ID; the hit-group contribution is dropped.
struct RepackParams {
    device const uint *src;
    device uint *dst;
    uint count;
};

kernel void equine_repack_instances(constant RepackParams &p [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i >= p.count) {
        return;
    }
    device const uint *in = p.src + i * 16;
    device uint *out = p.dst + i * 18;
    for (uint w = 0; w < 12; ++w) {
        out[w] = in[w];
    }
    out[12] = in[13] >> 24;        // options: the instance flags
    out[13] = in[12] >> 24;        // mask
    out[14] = 0;                   // intersection function table offset
    out[15] = in[12] & 0xFFFFFFu;  // user ID: InstanceID()
    out[16] = in[14];              // accelerationStructureID
    out[17] = in[15];
}
)";

constexpr uint32 kCountGroupSize = 64;

struct CountParams {
    uint64 src, dst, count;
    uint32 total, words, zeroWord, maxCount;
};

struct GroupsParams {
    uint64 records, groups;
    uint32 words, argsWord, maxCount, vertexStride, overlap;
};

// A vertex function run with rasterization off, inside sections.
MTL::RenderPipelineState *vertexOnly(DeviceState &state, MTL::Function *vertex, const char *label) {
    MTL::RenderPipelineDescriptor *desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vertex);
    desc->setRasterizationEnabled(false);
    desc->setLabel(nsString(label));
    NS::Error *error = nullptr;
    MTL::RenderPipelineState *pipeline = state.device->newRenderPipelineState(desc, &error);
    if (pipeline == nullptr) {
        reportNsError(ErrorCategory::eUnsupported, std::format("{}: newRenderPipelineState", label), error);
    }
    desc->release();
    return pipeline;
}

MTL::Function *function(MTL::Library *library, const char *name) {
    MTL::Function *f = library->newFunction(nsString(name));
    if (f == nullptr) {
        reportError(ErrorCategory::eUnsupported, std::format("RI's count copy: function {} not found", name));
    }
    return f;
}

} // namespace

// RI's built-in MSL, compiled at first use (DeviceState::builtins).
const DeviceState::Builtins &builtinPipelines(DeviceState &state) {
    std::lock_guard lock(state.builtinsMutex);
    DeviceState::Builtins &b = state.builtins;
    if (b.countKernel != nullptr) {
        return b;
    }
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    NS::Error *error = nullptr;
    MTL::Library *library = state.device->newLibrary(nsString(kCountSource), nullptr, &error);
    if (library == nullptr) {
        reportNsError(ErrorCategory::eUnsupported, "RI's count copy: newLibrary", error);
    }
    MTL::Function *kernel = function(library, "equine_count_kernel");
    MTL::Function *vertex = function(library, "equine_count_vertex");
    MTL::Function *repack = function(library, "equine_repack_instances");
    MTL::Function *groups = function(library, "equine_emulation_groups");
    b.repackKernel = state.device->newComputePipelineState(repack, &error);
    if (b.repackKernel == nullptr) {
        reportNsError(ErrorCategory::eUnsupported, "RI's instance repack: newComputePipelineState", error);
    }
    repack->release();
    b.countKernel = state.device->newComputePipelineState(kernel, &error);
    if (b.countKernel == nullptr) {
        reportNsError(ErrorCategory::eUnsupported, "RI's count copy: newComputePipelineState", error);
    }
    b.countVertex = vertexOnly(state, vertex, "EquineRI count copy");
    b.emulationGroups = vertexOnly(state, groups, "EquineRI emulation groups");
    groups->release();
    kernel->release();
    vertex->release();
    library->release();
    pool->release();
    return b;
}

namespace {

// The records RI's indirect commands read: `args` itself, or a copy in
// command memory with the records at or beyond the count emptied.
uint64 recordsAddress(DeviceState &state, CommandListState &list, IndirectKind kind, uint64 args, uint32 stride,
                      uint32 rootSize, uint32 maxCount, const Buffer *count, uint64 countOffset) {
    if (count == nullptr) {
        return args;
    }
    const uint32 words = stride / 4;
    // Draws: the instance count; dispatches and mesh draws: the group count x.
    const bool draw = kind == IndirectKind::eDraw || kind == IndirectKind::eDrawIndexed;
    const uint32 zeroWord = rootSize / 4 + (draw ? 1 : 0);
    const uint64 bytes = uint64{stride} * maxCount;
    const CommandMemoryAllocation copy = allocateCommandMemory(state, list, bytes, 16);
    const CommandMemoryAllocation params = allocateCommandMemory(state, list, sizeof(CountParams), 16);
    const CountParams p{.src = args,
                        .dst = copy.gpuAddress,
                        .count = count->gpuAddress + countOffset,
                        .total = words * maxCount,
                        .words = words,
                        .zeroWord = zeroWord,
                        .maxCount = maxCount};
    std::memcpy(params.cpu, &p, sizeof(p));
    const DeviceState::Builtins &b = builtinPipelines(state);
    list.argumentTable->setAddress(params.gpuAddress, kResourceHeapIndex);
    if (list.render != nullptr) {
        list.render->setRenderPipelineState(b.countVertex);
        list.render->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger{0}, NS::UInteger{p.total});
        list.render->barrierAfterEncoderStages(MTL::StageVertex, MTL::StageVertex | MTL::StageObject | MTL::StageMesh,
                                               MTL4::VisibilityOptionDevice);
        list.render->setRenderPipelineState(static_cast<MTL::RenderPipelineState *>(list.graphics.internal.pipelineState));
    } else {
        MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);
        encoder->setComputePipelineState(b.countKernel);
        encoder->dispatchThreadgroups(MTL::Size((p.total + kCountGroupSize - 1) / kCountGroupSize, 1, 1),
                                      MTL::Size(kCountGroupSize, 1, 1));
        encoder->barrierAfterEncoderStages(MTL::StageDispatch, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
        if (list.pipeline != nullptr) {
            encoder->setComputePipelineState(list.pipeline);
        }
    }
    if (list.resourceHeapAddress != 0) {
        list.argumentTable->setAddress(list.resourceHeapAddress, kResourceHeapIndex);
    }
    return copy.gpuAddress;
}

void execute(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
             const Buffer *count, uint64 countOffset, bool level2, const char *call) {
    DeviceState &state = *cmd.internal.device;
    CommandListState &list = *cmd.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, args.internal.id, "buffer");
    if (count != nullptr) {
        EQ_MTL_CHECK_ALIVE(&state, count->internal.id, "buffer");
    }
    const bool dispatch = kind == IndirectKind::eDispatch;
    const uint32 rootSize = level2 ? (list.rootDataSize + 3) / 4 * 4 : 0;
    const uint32 stride = indirectStride(kind, rootSize);
    EQ_VALIDATE(Validation::checkIndirect(
        {.dispatch = dispatch,
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
    // Metal faults on a draw without a section or pipeline: every build.
    if (dispatch) {
        if (list.rendering) {
            reportError(ErrorCategory::eProgrammerError,
                        std::format("{}: dispatches run outside rendering sections: call cmdEndRendering first", call));
        }
        if (list.pipeline == nullptr) {
            reportError(ErrorCategory::eProgrammerError, std::format("{}: no compute pipeline is set (cmdSetPipeline)", call));
        }
    } else {
        checkDraw(list, call, kind == IndirectKind::eDrawMesh);
        if (kind == IndirectKind::eDrawIndexed && !list.indexSet) {
            reportError(ErrorCategory::eProgrammerError, std::format("{}: no index buffer is set (cmdSetIndexBuffer)", call));
        }
    }
    if (maxCount == 0) {
        return;
    }
    const uint64 records =
        recordsAddress(state, list, kind, args.gpuAddress + offset, stride, rootSize, maxCount, count, countOffset);
    if (!dispatch && list.graphics.internal.emulation != 0) {
        list.sectionDrawn = true;
        drawEmulatedIndirect(state, list, kind == IndirectKind::eDrawIndexed, records, stride, rootSize, maxCount,
                             level2);
        return;
    }
    const auto &p = list.graphics.internal;
    if (!dispatch) {
        bindIndexType(state, list, kind == IndirectKind::eDrawIndexed ? (list.indexFormat == IndexFormat::eUint32 ? kIndexUint32 : kIndexUint16) : kNonIndexed);
        list.sectionDrawn = true;
    }
    for (uint32 i = 0; i < maxCount; ++i) {
        const uint64 record = records + uint64{i} * stride;
        const uint64 arguments = record + rootSize;
        if (level2) {
            list.argumentTable->setAddress(record, kRootDataIndex);
        }
        switch (kind) {
        case IndirectKind::eDraw:
            list.argumentTable->setAddress(arguments, kDrawArgumentsIndex);
            list.render->drawPrimitives(static_cast<MTL::PrimitiveType>(p.primitiveType), arguments);
            break;
        case IndirectKind::eDrawIndexed:
            list.argumentTable->setAddress(arguments, kDrawArgumentsIndex);
            list.render->drawIndexedPrimitives(static_cast<MTL::PrimitiveType>(p.primitiveType),
                                               list.indexFormat == IndexFormat::eUint32 ? MTL::IndexTypeUInt32
                                                                                        : MTL::IndexTypeUInt16,
                                               list.indexAddress, list.indexLength, arguments);
            break;
        case IndirectKind::eDrawMesh:
            list.render->drawMeshThreadgroups(
                arguments, MTL::Size(p.taskThreadGroupSize[0], p.taskThreadGroupSize[1], p.taskThreadGroupSize[2]),
                MTL::Size(p.threadGroupSize[0], p.threadGroupSize[1], p.threadGroupSize[2]));
            break;
        case IndirectKind::eDispatch:
            computeEncoder(list)->dispatchThreadgroups(
                arguments, MTL::Size(list.threadGroupSize[0], list.threadGroupSize[1], list.threadGroupSize[2]));
            break;
        }
    }
}

} // namespace

uint64 emulationGroups(DeviceState &state, CommandListState &cmd, uint64 records, uint32 stride, uint32 rootSize,
                       uint32 maxCount, uint32 vertexStride, uint32 overlap) {
    const CommandMemoryAllocation groups = allocateCommandMemory(state, cmd, uint64{12} * maxCount, 16);
    const CommandMemoryAllocation params = allocateCommandMemory(state, cmd, sizeof(GroupsParams), 16);
    const GroupsParams p{.records = records,
                         .groups = groups.gpuAddress,
                         .words = stride / 4,
                         .argsWord = rootSize / 4,
                         .maxCount = maxCount,
                         .vertexStride = vertexStride,
                         .overlap = overlap};
    std::memcpy(params.cpu, &p, sizeof(p));
    const DeviceState::Builtins &b = builtinPipelines(state);
    cmd.argumentTable->setAddress(params.gpuAddress, kResourceHeapIndex);
    cmd.render->setRenderPipelineState(b.emulationGroups);
    cmd.render->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger{0}, NS::UInteger{maxCount});
    // The mesh draws read the counts as indirect arguments.
    cmd.render->barrierAfterEncoderStages(MTL::StageVertex, MTL::StageVertex | MTL::StageObject | MTL::StageMesh,
                                          MTL4::VisibilityOptionDevice);
    cmd.render->setRenderPipelineState(static_cast<MTL::RenderPipelineState *>(cmd.graphics.internal.pipelineState));
    if (cmd.resourceHeapAddress != 0) {
        cmd.argumentTable->setAddress(cmd.resourceHeapAddress, kResourceHeapIndex);
    }
    return groups.gpuAddress;
}

void releaseBuiltins(DeviceState &state) {
    release(state.builtins.countKernel);
    release(state.builtins.countVertex);
    release(state.builtins.emulationGroups);
    release(state.builtins.repackKernel);
    release(state.builtins.emptyFragment);
}

void cmdExecuteIndirect(CommandList &cmd, IndirectKind kind, const Buffer &args, uint64 offset, uint32 maxCount,
                        const Buffer *count, uint64 countOffset) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    execute(cmd, kind, args, offset, maxCount, count, countOffset, false, "cmdExecuteIndirect");
}

// Records are read where they are (or from command memory with a count): no scratch.
MemoryRequirements indirectScratchRequirements(Device &device, [[maybe_unused]] const Pipeline &pipeline, IndirectKind,
                                               uint32) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    EQ_MTL_CHECK_ALIVE(device.internal.state, pipeline.internal.id, "pipeline");
    return {.size = 0, .alignment = 1, .kinds = bufferRequirements(device, {.size = 256}).kinds};
}

void cmdExecuteIndirectWithRootData(CommandList &cmd, IndirectKind kind, const Buffer &records, uint64 offset,
                                    uint32 maxCount, const Buffer *scratch, uint64, const Buffer *count,
                                    uint64 countOffset) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    if (scratch != nullptr) {
        EQ_MTL_CHECK_ALIVE(cmd.internal.device, scratch->internal.id, "buffer");
    }
    execute(cmd, kind, records, offset, maxCount, count, countOffset, true, "cmdExecuteIndirectWithRootData");
}

} // namespace RI::Metal
