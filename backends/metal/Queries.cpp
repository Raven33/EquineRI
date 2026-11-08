// Queries on Metal (P51, P71, P116).
//
// Owns: query pools (timestamp pools are MTL4CounterHeaps of timestamp type),
// resetQueries, pipelineStatisticsLayout, and the timestamp commands
// (cmdWriteTimestamp, cmdResetQueries, cmdResolveQueries). cmdBeginQuery and
// cmdEndQuery wait for occlusion queries, with rendering (Unsupported.cpp).
//
// Watch out: Metal 4 has no pipeline-statistics counters at all (P116), and
// occlusion queries (visibility result buffers) are not built yet: their pools
// report eUnsupported. A counter heap is reset on the CPU by invalidating its
// entries; that is RI's reset (P71 (1)) and costs no GPU work, and
// cmdResetQueries has nothing to do, as on D3D12. Timestamps are written on
// the open compute encoder after barriers on every earlier stage, so they mark
// the end of all earlier work. A resolve is a command-buffer command outside
// encoders, which barriers do not order: the device's resolve fence orders it
// after earlier work (updated by the encoder before it) and later encoders
// after it (computeEncoder waits for it).
//
// D3D12 counterpart: the query section of backends/d3d12/Textures.cpp.

#include "DeviceState.hpp"
#include <RI/Validation/Checks.hpp>

#include <algorithm>
#include <format>

namespace RI::Metal {

namespace {

MTL4::CounterHeap *heapOf(const QueryPool &pool) { return static_cast<MTL4::CounterHeap *>(pool.internal.heap); }

} // namespace

QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    if (desc.type == QueryType::eOcclusion) {
        reportError(ErrorCategory::eUnsupported,
                    "createQueryPool: occlusion queries are not built yet on the Metal backend (they come with "
                    "rendering; see docs/FeatureProposal-Metal-2026-11-07.md)");
    }
    if (desc.type != QueryType::eTimestamp) {
        reportError(ErrorCategory::eUnsupported,
                    "createQueryPool: Metal 4 has no pipeline statistics counters (P116)");
    }

    MTL4::CounterHeap *heap = nullptr;
    {
        NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
        MTL4::CounterHeapDescriptor *heapDesc = MTL4::CounterHeapDescriptor::alloc()->init();
        heapDesc->setType(MTL4::CounterHeapTypeTimestamp);
        // Metal rejects empty heaps; an empty pool keeps one unused entry, as D3D12's does.
        heapDesc->setCount(std::max(desc.count, 1u));
        NS::Error *error = nullptr;
        heap = state.device->newCounterHeap(heapDesc, &error);
        heapDesc->release();
        if (heap == nullptr) {
            reportNsError(ErrorCategory::eOutOfMemory, "newCounterHeap", error);
        }
        pool->release();
    }
    // Created reset (P71 (1)): invalidated entries resolve as unwritten.
    heap->invalidateCounterRange(NS::Range(0, std::max(desc.count, 1u)));

    QueryPool out;
    out.type = desc.type;
    out.count = desc.count;
    out.internal.device = &state;
    out.internal.heap = heap;
    out.internal.id = EQ_MTL_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(state, out, desc.name);
    }
    return out;
}

void destroyQueryPool(Device &device, QueryPool &pool) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_MTL_UNTRACK(&state, pool.internal.id);
    MTL4::CounterHeap *heap = heapOf(pool);
    release(heap);
    pool = {};
}

void resetQueries(Device &device, QueryPool &pool, uint32 first, uint32 count) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "resetQueries: the query range"));
    if (count != 0) {
        heapOf(pool)->invalidateCounterRange(NS::Range(first, count));
    }
}

// Timestamps and occlusion queries: one 64-bit value each, no counters.
// Metal has no pipeline statistics (P116): pools of the counter types cannot
// be created, so their results have no size.
// Any thread (P106): a pure query.
PipelineStatisticsLayout pipelineStatisticsLayout(Device &, QueryType type) {
    const bool single = type == QueryType::eTimestamp || type == QueryType::eOcclusion;
    return {.size = single ? 8u : 0u,
            .inputVertices = kNoCounter,
            .inputPrimitives = kNoCounter,
            .vertexShaderInvocations = kNoCounter,
            .clippingInvocations = kNoCounter,
            .clippingPrimitives = kNoCounter,
            .pixelShaderInvocations = kNoCounter,
            .computeShaderInvocations = kNoCounter,
            .geometryShaderInvocations = kNoCounter,
            .geometryShaderPrimitives = kNoCounter,
            .hullShaderInvocations = kNoCounter,
            .domainShaderInvocations = kNoCounter,
            .meshPrimitives = kNoCounter};
}

// ── Timestamp commands (P51, P71) ────────────────────────────────────────────

namespace {

constexpr uint64 kComputeStages = MTL::StageDispatch | MTL::StageBlit;

// The open encoder, after every earlier stage of this encoder and of the
// encoders before it: what follows starts once all earlier work is done.
MTL4::ComputeCommandEncoder *afterEarlierWork(CommandListState &cmd) {
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(cmd);
    encoder->barrierAfterQueueStages(MTL::StageAll, kComputeStages, MTL4::VisibilityOptionDevice);
    encoder->barrierAfterEncoderStages(kComputeStages, kComputeStages, MTL4::VisibilityOptionDevice);
    return encoder;
}

} // namespace

void cmdWriteTimestamp(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type == QueryType::eTimestamp, "cmdWriteTimestamp"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdWriteTimestamp: the query"));
    CommandListState &list = *cmd.internal.state;
    // Rendering sections are not built yet: timestamps go on the compute encoder.
    afterEarlierWork(list)->writeTimestamp(MTL4::TimestampGranularityPrecise, heapOf(pool), index);
}

// Counter heaps need no reset on the GPU (P71 (1)): a write replaces the entry.
void cmdResetQueries([[maybe_unused]] CommandList &cmd, [[maybe_unused]] const QueryPool &pool,
                     [[maybe_unused]] uint32 first, [[maybe_unused]] uint32 count) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.state->rendering, false, "cmdResetQueries"));
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "cmdResetQueries: the query range"));
}

void cmdResolveQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count, const Buffer &buffer,
                       uint64 offset) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdResolveQueries"));
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "cmdResolveQueries: the query range"));
    EQ_VALIDATE(Validation::checkRange(offset, uint64{count} * 8, buffer.size, 8,
                                       "cmdResolveQueries: the results in the buffer"));
    if ((buffer.usage & BufferUsage::eCopyDst) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError, "cmdResolveQueries: the buffer needs BufferUsage::eCopyDst");
    }
    if (count == 0) {
        return;
    }
    // Timestamp pools only (occlusion pools cannot be created yet): one 64-bit
    // tick count per query, MTL4TimestampHeapEntry.
    afterEarlierWork(list)->updateFence(list.resolveFence, kComputeStages);
    endEncoder(list);
    list.commandBuffer->resolveCounterHeap(heapOf(pool), NS::Range(first, count),
                                           MTL4::BufferRange(buffer.gpuAddress + offset, uint64{count} * 8),
                                           list.resolveFence, list.resolveFence);
    list.waitResolve = true;
}

} // namespace RI::Metal
