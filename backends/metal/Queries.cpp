// Queries on Metal (P51, P71, P116).
//
// Owns: query pools (timestamp pools are MTL4CounterHeaps of timestamp type,
// occlusion pools a private buffer of one 64-bit count per query),
// resetQueries, pipelineStatisticsLayout, the timestamp commands
// (cmdWriteTimestamp, cmdResetQueries, cmdResolveQueries), and occlusion
// queries (cmdBeginQuery, cmdEndQuery, copyOcclusionResults, P115).
//
// Keeps: in each command list, the slots the open section's occlusion
// queries took in its visibility page (CommandListState::occlusionSlots).
//
// Watch out: Metal 4 has no pipeline-statistics counters at all (P116): those
// pools report eUnsupported. Occlusion queries are visibility results: a
// render encoder counts the samples passing depth and stencil into one 8-byte
// slot at a time (setVisibilityResultMode Counting), and only inside its own
// render pass, so on Metal a query begins and ends inside one section, and
// one is open at a time (as on Vulkan). Each section points its visibility
// buffer at a page of kOcclusionSlotsPerSection slots of command memory
// (Rendering.cpp); cmdBeginQuery takes the next slot and cmdEndRendering
// copies the used slots into the pools, so a pool's results outlive the
// page. A slot named while disabling is written too (probed), so cmdEndQuery
// disables on the query's own slot. A counter heap is reset on the CPU by invalidating its
// entries; that is RI's reset (P71 (1)) and costs no GPU work, and
// cmdResetQueries has nothing to do, as on D3D12. Timestamps outside
// sections are the command buffer's (writeTimestampIntoHeap, after all earlier
// work; the open encoder ends first); inside a section, the render encoder's,
// after the stage the earlier draws finish in. A resolve is a command-buffer command outside
// encoders, which barriers do not order: the device's resolve fence orders it
// after earlier work (updated by the encoder before it) and later encoders
// after it (computeEncoder waits for it).
//
// D3D12 counterpart: the query section of backends/d3d12/Rendering.cpp.

#include "DeviceState.hpp"
#include <RI/Validation/Checks.hpp>

#include <algorithm>
#include <format>

namespace RI::Metal {

namespace {

MTL4::CounterHeap *heapOf(const QueryPool &pool) { return static_cast<MTL4::CounterHeap *>(pool.internal.heap); }
MTL::Buffer *resultsOf(const QueryPool &pool) { return static_cast<MTL::Buffer *>(pool.internal.heap); }

} // namespace

QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    if (desc.type == QueryType::eOcclusion) {
        // One 64-bit count per query, written by copies only: private storage.
        MTL::Buffer *buffer = state.device->newBuffer(uint64{std::max(desc.count, 1u)} * 8,
                                                      MTL::ResourceStorageModePrivate |
                                                          MTL::ResourceHazardTrackingModeUntracked);
        if (buffer == nullptr) {
            reportError(ErrorCategory::eOutOfMemory, "createQueryPool: newBuffer failed for the occlusion results");
        }
        makeResident(state, buffer);
        state.occlusionPools.fetch_add(1, std::memory_order_relaxed);
        QueryPool out;
        out.type = desc.type;
        out.count = desc.count;
        out.internal.device = &state;
        out.internal.heap = buffer;
        out.internal.id = EQ_MTL_TRACK(&state);
        if (!desc.name.empty()) {
            applyName(state, out, desc.name);
        }
        return out;
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
    if (pool.type == QueryType::eOcclusion) {
        MTL::Buffer *buffer = resultsOf(pool);
        makeNonResident(state, buffer);
        release(buffer);
        state.occlusionPools.fetch_sub(1, std::memory_order_relaxed);
    } else {
        MTL4::CounterHeap *heap = heapOf(pool);
        release(heap);
    }
    pool = {};
}

void resetQueries(Device &device, QueryPool &pool, uint32 first, uint32 count) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "resetQueries: the query range"));
    // Occlusion results are rewritten by each query's copy: nothing to reset.
    if (count != 0 && pool.type == QueryType::eTimestamp) {
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
    // Inside a section: after the fragment stage of the draws before it (Metal
    // then guarantees they are done). Before any draw, Metal writes a
    // fragment timestamp only once a draw runs (0 otherwise, probed on the
    // M4), so it goes after the vertex stage, which first waits for the
    // earlier encoders (the section's vertex work does not otherwise).
    if (list.render != nullptr) {
        if (list.sectionDrawn) {
            list.render->writeTimestamp(MTL4::TimestampGranularityPrecise, MTL::RenderStageFragment, heapOf(pool),
                                        index);
        } else {
            list.render->barrierAfterQueueStages(MTL::StageAll, MTL::StageVertex | MTL::StageObject | MTL::StageMesh,
                                                 MTL4::VisibilityOptionDevice);
            list.render->writeTimestamp(MTL4::TimestampGranularityPrecise, MTL::RenderStageVertex, heapOf(pool),
                                        index);
        }
        return;
    }
    // Outside sections, the command buffer's own timestamp, written once all
    // earlier work of the command buffer is done: encoder timestamps of
    // compute encoders came back 0 or out of order once a command list had
    // several encoders (probed on the M4).
    endEncoder(list);
    list.commandBuffer->writeTimestampIntoHeap(heapOf(pool), index);
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
    // Occlusion: a copy, after every earlier copy (those of cmdEndRendering
    // that wrote the pool, in this encoder or earlier ones).
    if (pool.type == QueryType::eOcclusion) {
        afterEarlierWork(list)->copyFromBuffer(resultsOf(pool), uint64{first} * 8,
                                               static_cast<MTL::Buffer *>(buffer.internal.buffer), offset,
                                               uint64{count} * 8);
        return;
    }
    // Timestamps: one 64-bit tick count per query, MTL4TimestampHeapEntry.
    afterEarlierWork(list)->updateFence(list.resolveFence, kComputeStages);
    endEncoder(list);
    list.commandBuffer->resolveCounterHeap(heapOf(pool), NS::Range(first, count),
                                           MTL4::BufferRange(buffer.gpuAddress + offset, uint64{count} * 8),
                                           list.resolveFence, list.resolveFence);
    list.waitResolve = true;
}

// ── Occlusion queries (P115) ─────────────────────────────────────────────────

namespace {

// Every build: Metal counts visibility only inside a render pass.
void requireOcclusionSection(const CommandListState &cmd, const char *call) {
    if (cmd.render == nullptr) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: on Metal, an occlusion query begins and ends inside one rendering section "
                                "(visibility results are render-pass state, P115)",
                                call));
    }
}

void checkOcclusionPool([[maybe_unused]] const CommandList &cmd, const QueryPool &pool, [[maybe_unused]] uint32 index,
                        const char *call) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type != QueryType::eTimestamp, call));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, std::format("{}: the query", call)));
    // Only occlusion pools reach here (createQueryPool refuses statistics):
    // a timestamp pool would be read as a buffer, so this holds in every build.
    if (pool.type != QueryType::eOcclusion) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: wrong query pool type (timestamps use "
                                                                 "cmdWriteTimestamp)",
                                                                 call));
    }
}

uint64 slotOffset(const CommandListState &cmd, uint32 slot) { return cmd.visibilityOffset + uint64{slot} * 8; }

} // namespace

void cmdBeginQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    checkOcclusionPool(cmd, pool, index, "cmdBeginQuery");
    CommandListState &list = *cmd.internal.state;
    requireOcclusionSection(list, "cmdBeginQuery");
    // Metal counts into one slot at a time; Vulkan allows one active query of a type.
    if (list.occlusionOpen) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdBeginQuery: an occlusion query is already open: end it first (one at a time)");
    }
    // The section's visibility buffer was set as it began, from the pools alive then.
    if (list.visibilityBuffer == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdBeginQuery: no occlusion query pool existed when this rendering section began; on Metal, "
                    "create occlusion pools before the sections that use them (P115)");
    }
    const auto slot = static_cast<uint32>(list.occlusionSlots.size());
    EQ_VALIDATE(slot < kOcclusionSlotsPerSection
                    ? Validation::CheckResult{}
                    : Validation::CheckResult{std::format(
                          "cmdBeginQuery: more than {} occlusion queries in one rendering section; Metal counts them "
                          "in a page of {} slots per section (P115): end the section and begin another",
                          kOcclusionSlotsPerSection, kOcclusionSlotsPerSection)});
    list.openOcclusion = {.pool = static_cast<MTL::Buffer *>(pool.internal.heap), .index = index, .slot = slot};
    list.occlusionOpen = true;
    // Other builds: queries past the page count nothing and leave their result unwritten.
    if (slot >= kOcclusionSlotsPerSection) {
        return;
    }
    list.occlusionSlots.push_back(list.openOcclusion);
    list.render->setVisibilityResultMode(MTL::VisibilityResultModeCounting, slotOffset(list, slot));
}

void cmdEndQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    checkOcclusionPool(cmd, pool, index, "cmdEndQuery");
    CommandListState &list = *cmd.internal.state;
    requireOcclusionSection(list, "cmdEndQuery");
    if (!list.occlusionOpen || list.openOcclusion.pool != pool.internal.heap || list.openOcclusion.index != index) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdEndQuery: this occlusion query is not the one open (cmdBeginQuery in this section)");
    }
    list.occlusionOpen = false;
    if (list.openOcclusion.slot >= kOcclusionSlotsPerSection) {
        return;
    }
    // Disabling names a slot too, which Metal writes: the query's own.
    list.render->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, slotOffset(list, list.openOcclusion.slot));
}

// After the section's encoder ended (its producer barrier orders this after
// the visibility writes): one copy per run of slots that land side by side in
// one pool, after every earlier copy (a resolve may still read the pool).
void copyOcclusionResults(CommandListState &cmd) {
    if (cmd.occlusionSlots.empty()) {
        return;
    }
    MTL4::ComputeCommandEncoder *encoder = afterEarlierWork(cmd);
    const std::vector<OcclusionSlot> &slots = cmd.occlusionSlots;
    for (size_t first = 0; first < slots.size();) {
        size_t end = first + 1;
        while (end < slots.size() && slots[end].pool == slots[first].pool &&
               slots[end].index == slots[first].index + (end - first)) {
            ++end;
        }
        encoder->copyFromBuffer(cmd.visibilityBuffer, slotOffset(cmd, slots[first].slot), slots[first].pool,
                                uint64{slots[first].index} * 8, (end - first) * 8);
        first = end;
    }
    cmd.occlusionSlots.clear();
}

} // namespace RI::Metal
