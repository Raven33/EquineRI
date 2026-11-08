// Command lists, binding, barriers, buffer commands and submission on Metal
// (P20, P34, P36, P56, P85, P107, P115).
//
// Owns: command lists (an MTL4CommandAllocator, an MTL4CommandBuffer and one
// MTL4ArgumentTable each), their command memory (allocateCommandMemory), the
// open compute encoder (computeEncoder / endEncoder), render passes that only
// clear (runClearPass), cmdBegin / cmdEnd,
// cmdSetHeap, cmdSetPipeline, cmdSetRootData, cmdDispatch, cmdBarrier (stage
// masks as MTLStages, stagesOf), cmdCopyBuffer, cmdClearBuffer,
// cmdWriteSmallData, submit, wait and isReached (an MTLSharedEvent as the
// timeline).
//
// Keeps: the bound heaps, pipeline and root data in the argument table, which
// Metal snapshots at each dispatch; the table is set on every compute encoder
// when it opens, so binding outlives encoder boundaries.
//
// Watch out: Metal 4 has no setBytes. Argument tables hold addresses whose
// bytes the GPU reads at execution, so root data, small writes and clear
// patterns are copied into command memory, which lives until the list is
// reset (P115). A compute encoder accepts only Dispatch, Blit and
// AccelerationStructure stages in its own barriers; barriers that involve
// other encoders go through queue barriers: a consumer barrier when an encoder
// opens after barriers recorded with none open or in the encoder before it
// (so it waits for every earlier encoder, not only that one), and a producer
// barrier when an encoder that recorded barriers closes, which also orders
// later submissions. Commit feedback arrives after the timeline event: wait
// and isReached count a value as reached once both are (GpuFeedback).
//
// D3D12 counterpart: backends/d3d12/Commands.cpp (and cmdClearBuffer /
// cmdWriteSmallData in backends/d3d12/Rendering.cpp).

#include "DeviceState.hpp"

#include <RI/Metal/Native.hpp>
#include <RI/Validation/Checks.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

// Calls that create autoreleased objects drain their own pool (DeviceState.hpp).
struct ScopedPool {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    ~ScopedPool() { pool->release(); }
};

constexpr uint64 kFirstChunkSize = 64 * 1024;
// Bytes of the value written into command memory by a pattern clear; the rest
// of the range is filled by copies that double inside the destination.
constexpr uint64 kClearPatternSize = 4096;
// The stages a compute encoder runs and accepts in its own barriers.
constexpr uint64 kComputeEncoderStages = MTL::StageDispatch | MTL::StageBlit | MTL::StageAccelerationStructure;
// How long one wait blocks before looking for a GPU error again.
constexpr uint64 kWaitSliceMs = 100;

constexpr MTL::ResourceOptions kCommandMemoryOptions =
    MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined | MTL::ResourceHazardTrackingModeUntracked;

MTL::Stages mtlStages(uint64 mask) { return static_cast<MTL::Stages>(mask); }

// A barrier side inside the open compute encoder. A side left empty names
// only stages the encoder does not run (graphics work ordered within this
// encoder's scope), so it orders the encoder's own work conservatively.
uint64 encoderSide(uint64 stages) {
    const uint64 restricted = stages & kComputeEncoderStages;
    return restricted != 0 ? restricted : (MTL::StageDispatch | MTL::StageBlit);
}

void checkNotRendering(const CommandListState &cmd, const char *call) {
    if (cmd.rendering) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: not allowed inside a rendering section", call));
    }
}

// Command memory starts over once the GPU is done with the list (P115).
void recycle(CommandListState &cmd) {
    cmd.allocator->reset();
    cmd.chunk = 0;
    cmd.used = 0;
}

} // namespace

// ── Shared helpers (DeviceState.hpp) ─────────────────────────────────────────

uint64 completedValue(DeviceState &state) {
    const uint64 signaled = state.timeline->signaledValue();
    const std::lock_guard lock(state.feedback->mutex);
    return std::min(signaled, state.feedback->through);
}

uint64 stagesOf(Stage stage) {
    if ((stage & Stage::eAll) != Stage::eNone) {
        return MTL::StageAll;
    }
    uint64 stages = 0;
    // Clears and small writes outside rendering sections count as copies (P65, P107).
    if ((stage & Stage::eCopy) != Stage::eNone) {
        stages |= MTL::StageBlit;
    }
    if ((stage & Stage::eCompute) != Stage::eNone) {
        stages |= MTL::StageDispatch;
    }
    // Metal reads indirect arguments at the stage that consumes them.
    if ((stage & Stage::eIndirect) != Stage::eNone) {
        stages |= MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh;
    }
    if ((stage & Stage::eVertex) != Stage::eNone) {
        stages |= MTL::StageVertex | MTL::StageObject | MTL::StageMesh;
    }
    // Attachments are fragment work in Metal.
    if ((stage & (Stage::ePixel | Stage::eRenderTarget | Stage::eDepthStencil)) != Stage::eNone) {
        stages |= MTL::StageFragment;
    }
    if ((stage & Stage::eAccelerationStructure) != Stage::eNone) {
        stages |= MTL::StageAccelerationStructure;
    }
    return stages;
}

MTL4::ComputeCommandEncoder *computeEncoder(CommandListState &cmd) {
    if (cmd.compute != nullptr) {
        return cmd.compute;
    }
    {
        // The encoder is autoreleased: RI keeps its own reference until endEncoder.
        ScopedPool scope;
        cmd.compute = cmd.commandBuffer->computeCommandEncoder();
        cmd.compute->retain();
    }
    cmd.compute->setArgumentTable(cmd.argumentTable);
    if (cmd.pipeline != nullptr) {
        cmd.compute->setComputePipelineState(cmd.pipeline);
    }
    // Barriers recorded while no encoder was open: this encoder's work waits for
    // the earlier encoders' (of this list and of earlier submissions).
    if (cmd.pendingBefore != 0) {
        cmd.compute->barrierAfterQueueStages(mtlStages(cmd.pendingBefore), mtlStages(encoderSide(cmd.pendingAfter)),
                                             MTL4::VisibilityOptionDevice);
        cmd.pendingBefore = 0;
        cmd.pendingAfter = 0;
    }
    // A resolve runs outside encoders: queue barriers do not see it, its fence does.
    if (cmd.waitResolve) {
        cmd.compute->waitForFence(cmd.resolveFence, MTL::StageDispatch | MTL::StageBlit);
        cmd.waitResolve = false;
    }
    return cmd.compute;
}

void runClearPass(CommandListState &cmd, MTL4::RenderPassDescriptor *pass) {
    endEncoder(cmd);
    ScopedPool scope;
    MTL4::RenderCommandEncoder *encoder = cmd.commandBuffer->renderCommandEncoder(pass);
    // The barriers recorded before the clear also hold for it; they stay
    // pending for the next compute encoder too, which they were recorded for.
    if (cmd.pendingBefore != 0) {
        encoder->barrierAfterQueueStages(mtlStages(cmd.pendingBefore), mtlStages(kRenderEncoderStages),
                                         MTL4::VisibilityOptionDevice);
    }
    // A clear counts as Stage::eCopy (P65), which RI maps to blit stages: every
    // later encoder waits for this pass's fragment work instead.
    encoder->barrierAfterStages(mtlStages(kRenderEncoderStages), MTL::StageAll, MTL4::VisibilityOptionDevice);
    encoder->endEncoding();
}

void endEncoder(CommandListState &cmd) {
    if (cmd.compute == nullptr) {
        return;
    }
    // The barriers recorded in this encoder also hold for later encoders, of
    // this list and of later submissions: a producer barrier carries them past
    // this encoder's own work, and the next encoder of this list opens with a
    // consumer barrier, which also waits for encoders before this one.
    if (cmd.encoderBefore != 0) {
        cmd.compute->barrierAfterStages(mtlStages(cmd.encoderBefore), mtlStages(cmd.encoderAfter),
                                        MTL4::VisibilityOptionDevice);
        cmd.pendingBefore |= cmd.encoderBefore;
        cmd.pendingAfter |= cmd.encoderAfter;
    }
    cmd.encoderBefore = 0;
    cmd.encoderAfter = 0;
    cmd.compute->endEncoding();
    release(cmd.compute);
}

CommandMemoryAllocation allocateCommandMemory(DeviceState &state, CommandListState &cmd, uint64 size,
                                              uint64 alignment) {
    const auto fits = [&](size_t chunk, uint64 used) {
        return ((used + alignment - 1) & ~(alignment - 1)) + size <= cmd.chunks[chunk].size;
    };
    if (cmd.chunk < cmd.chunks.size() && !fits(cmd.chunk, cmd.used)) {
        // Later chunks are free since the last recycle; skip those too small.
        ++cmd.chunk;
        cmd.used = 0;
        while (cmd.chunk < cmd.chunks.size() && !fits(cmd.chunk, 0)) {
            ++cmd.chunk;
        }
    }
    if (cmd.chunk >= cmd.chunks.size()) {
        uint64 chunkSize = cmd.chunks.empty() ? kFirstChunkSize : cmd.chunks.back().size * 2;
        while (chunkSize < size + alignment) {
            chunkSize *= 2;
        }
        MTL::Buffer *buffer = state.device->newBuffer(chunkSize, kCommandMemoryOptions);
        if (buffer == nullptr) {
            reportError(ErrorCategory::eOutOfMemory,
                        std::format("command memory: newBuffer failed for a chunk of {} bytes", chunkSize));
        }
        makeResident(state, buffer);
        cmd.chunks.push_back({.buffer = buffer, .size = chunkSize});
        cmd.chunk = cmd.chunks.size() - 1;
        cmd.used = 0;
    }
    const CommandMemoryChunk &chunk = cmd.chunks[cmd.chunk];
    const uint64 offset = (cmd.used + alignment - 1) & ~(alignment - 1);
    cmd.used = offset + size;
    return {.cpu = static_cast<std::byte *>(chunk.buffer->contents()) + offset,
            .gpuAddress = chunk.buffer->gpuAddress() + offset,
            .buffer = chunk.buffer,
            .offset = offset};
}

// ── Command lists (P56) ──────────────────────────────────────────────────────

CommandList createCommandList(Device &device, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    auto *list = new CommandListState;
    list->resolveFence = state.resolveFence;
    list->allocator = state.device->newCommandAllocator();
    list->commandBuffer = state.device->newCommandBuffer();
    if (list->allocator == nullptr || list->commandBuffer == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "createCommandList: newCommandAllocator or newCommandBuffer failed");
    }
    {
        ScopedPool scope;
        MTL4::ArgumentTableDescriptor *desc = MTL4::ArgumentTableDescriptor::alloc()->init();
        desc->setMaxBufferBindCount(kArgumentTableBuffers);
        NS::Error *error = nullptr;
        list->argumentTable = state.device->newArgumentTable(desc, &error);
        desc->release();
        if (list->argumentTable == nullptr) {
            reportNsError(ErrorCategory::eOutOfMemory, "newArgumentTable", error);
        }
    }
    // MSC's index-type uniform is the same for every list: bound once.
    if (state.uniforms != nullptr) {
        list->argumentTable->setAddress(state.uniforms->gpuAddress(), kUniformsIndex);
    }

    CommandList cmd;
    cmd.internal.device = &state;
    cmd.internal.state = list;
    cmd.internal.id = EQ_MTL_TRACK(&state);
    if (!name.empty()) {
        applyName(state, cmd, name);
    }
    return cmd;
}

void destroyCommandList(Device &device, CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "destroying"));
    EQ_MTL_UNTRACK(&state, cmd.internal.id);
    CommandListState *list = cmd.internal.state;
    // A list destroyed while recording: Metal wants its encoder and command buffer ended.
    endEncoder(*list);
    if (list->recording) {
        list->commandBuffer->endCommandBuffer();
    }
    for (CommandMemoryChunk &chunk : list->chunks) {
        makeNonResident(state, chunk.buffer);
        release(chunk.buffer);
    }
    release(list->argumentTable);
    release(list->commandBuffer);
    release(list->allocator);
    delete list;
    cmd = {};
}

void resetCommandList(CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    CommandListState &list = *cmd.internal.state;
    // Metal faults on an allocator reset under an open command buffer: every build.
    if (list.recording) {
        reportError(ErrorCategory::eProgrammerError,
                    "resetCommandList: the command list is still recording: call cmdEnd first");
    }
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "resetting"));
    recycle(list);
    list.recorded = false; // the reset frees the recorded commands
}

// ── Recording (P34: each call writes the native command at once) ─────────────

void cmdBegin(CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, cmd.internal.id, "command list");
    CommandListState &list = *cmd.internal.state;
    // Metal asserts on a command buffer begun twice, and the open encoder
    // would take the new commands: every build, as submit's check.
    if (list.recording) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdBegin: the command list is already recording: call cmdEnd first");
    }
    // Re-recorded without a reset: command memory starts over only once the GPU
    // is done with it, otherwise it keeps growing until the next reset.
    if (cmd.internal.lastSubmitted <= completedValue(*cmd.internal.device)) {
        recycle(list);
    }
    list.commandBuffer->beginCommandBuffer(list.allocator);
    list.recording = true;
    list.recorded = false;
    list.heapSet = false;
    list.resourceHeapAddress = 0;
    list.samplerHeapAddress = 0;
    list.pipeline = nullptr;
    list.threadGroupSize[0] = list.threadGroupSize[1] = list.threadGroupSize[2] = 1;
    list.rootDataSize = 0;
    list.rendering = false;
    list.waitResolve = false;
    list.markerDepth = 0;
    list.pendingBefore = list.pendingAfter = 0;
    list.encoderBefore = list.encoderAfter = 0;
}

// Writes are visible to the CPU once the timeline value is reached: shared
// storage is coherent after the command buffer completes.
void cmdEnd(CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    if (!list.recording) {
        reportError(ErrorCategory::eProgrammerError, "cmdEnd: the command list is not recording: call cmdBegin first");
    }
    EQ_VALIDATE(Validation::checkMarkersClosed(list.markerDepth));
    // Barriers recorded after the last encoder still order later submissions:
    // an empty encoder takes them and its producer barrier passes them on. A
    // last resolve is waited for the same way.
    if (list.pendingBefore != 0 || list.waitResolve) {
        const uint64 before = list.pendingBefore | (list.waitResolve ? MTL::StageBlit | MTL::StageDispatch : 0);
        const uint64 after = list.pendingAfter | (list.waitResolve ? MTL::StageAll : 0);
        computeEncoder(list);
        list.encoderBefore |= before;
        list.encoderAfter |= after;
    }
    endEncoder(list);
    list.commandBuffer->endCommandBuffer();
    // Encoders ended: carried barriers start over with the next cmdBegin.
    list.pendingBefore = list.pendingAfter = 0;
    list.recording = false;
    list.recorded = true;
}

void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, heap.internal.id, "descriptor heap");
    CommandListState &list = *cmd.internal.state;
    const auto *resources = static_cast<const MTL::Buffer *>(heap.internal.resourceBuffer);
    const auto *samplers = static_cast<const MTL::Buffer *>(heap.internal.samplerBuffer);
    list.resourceHeapAddress = resources != nullptr ? resources->gpuAddress() : 0;
    list.samplerHeapAddress = samplers != nullptr ? samplers->gpuAddress() : 0;
    // The table is the open encoder's too: the next dispatch sees the new heaps.
    if (list.resourceHeapAddress != 0) {
        list.argumentTable->setAddress(list.resourceHeapAddress, kResourceHeapIndex);
    }
    if (list.samplerHeapAddress != 0) {
        list.argumentTable->setAddress(list.samplerHeapAddress, kSamplerHeapIndex);
    }
    list.heapSet = true;
}

void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, pipeline.internal.id, "pipeline");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkHeapSetBeforePipeline(list.heapSet));
    if (pipeline.internal.graphics) {
        notYet("cmdSetPipeline with a graphics pipeline");
    }
    list.pipeline = static_cast<MTL::ComputePipelineState *>(pipeline.internal.pipelineState);
    std::copy_n(pipeline.internal.threadGroupSize, 3, list.threadGroupSize);
    list.rootDataSize = pipeline.rootDataSize;
    // Otherwise set when the next compute encoder opens.
    if (list.compute != nullptr) {
        list.compute->setComputePipelineState(list.pipeline);
    }
}

// Root data is the shader's top-level argument buffer (P114): its bytes go to
// command memory and index 2 of the table points at them.
void cmdSetRootData(CommandList &cmd, std::span<const std::byte> data) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRootDataSize(data.size(), list.rootDataSize));
    if (data.empty()) {
        return;
    }
    const CommandMemoryAllocation memory = allocateCommandMemory(*cmd.internal.device, list, data.size(), 16);
    std::memcpy(memory.cpu, data.data(), data.size());
    list.argumentTable->setAddress(memory.gpuAddress, kRootDataIndex);
}

void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkGroupCounts(x, y, z, false, "cmdDispatch"));
    // Metal would dereference a nil pipeline state, so this holds in every build.
    if (list.pipeline == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "cmdDispatch: no compute pipeline is set (cmdSetPipeline)");
    }
    if (x == 0 || y == 0 || z == 0) {
        return; // no work, as an empty Dispatch on the other backends
    }
    computeEncoder(list)->dispatchThreadgroups(
        MTL::Size(x, y, z), MTL::Size(list.threadGroupSize[0], list.threadGroupSize[1], list.threadGroupSize[2]));
}

// Coarse stage -> stage barrier (P36), recorded where it stands (proposal §6.2).
void cmdBarrier(CommandList &cmd, Stage before, Stage after) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    // Ray tracing is not built yet on Metal (DeviceLimits::rayTracing is false).
    if (((before | after) & Stage::eAccelerationStructure) != Stage::eNone) {
        reportError(ErrorCategory::eUnsupported,
                    "cmdBarrier: Stage::eAccelerationStructure needs ray tracing (DeviceLimits::rayTracing)");
    }
    const uint64 producers = stagesOf(before);
    const uint64 consumers = stagesOf(after);
    if (producers == 0 || consumers == 0) {
        return;
    }
    CommandListState &list = *cmd.internal.state;
    if (list.compute == nullptr) {
        list.pendingBefore |= producers;
        list.pendingAfter |= consumers;
        return;
    }
    // Within the encoder, and against earlier encoders and submissions: an
    // encoder barrier orders only this encoder's own commands.
    list.compute->barrierAfterEncoderStages(mtlStages(encoderSide(producers)), mtlStages(encoderSide(consumers)),
                                            MTL4::VisibilityOptionDevice);
    list.compute->barrierAfterQueueStages(mtlStages(producers), mtlStages(encoderSide(consumers)),
                                          MTL4::VisibilityOptionDevice);
    list.encoderBefore |= producers;
    list.encoderAfter |= consumers;
}

void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, src.internal.id, "buffer");
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, dst.internal.id, "buffer");
    if (size == 0) {
        return;
    }
    computeEncoder(*cmd.internal.state)
        ->copyFromBuffer(static_cast<MTL::Buffer *>(src.internal.buffer), srcOffset,
                         static_cast<MTL::Buffer *>(dst.internal.buffer), dstOffset, size);
}

// ── Buffer clears and small writes (P65, P107) ───────────────────────────────

void cmdClearBuffer(CommandList &cmd, const Buffer &buffer, uint32 value, uint64 offset, uint64 size) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    CommandListState &list = *cmd.internal.state;
    checkNotRendering(list, "cmdClearBuffer");
    // The portable rule (P95 (7)), Vulkan's message. D3D12 also needs eGPU or
    // eGPUUpload memory, which Metal cannot tell from eUpload here.
    if ((buffer.usage & BufferUsage::eCopyDst) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError, "cmdClearBuffer: the buffer needs BufferUsage::eCopyDst");
    }
    EQ_VALIDATE(Validation::checkClearRange(offset, size, buffer.size));
    const uint64 bytes = size == kWholeSize ? buffer.size - offset : size;
    if (bytes == 0) {
        return;
    }
    auto *dst = static_cast<MTL::Buffer *>(buffer.internal.buffer);
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);
    const uint8 byte = value & 0xff;
    if (value == byte * 0x01010101u) {
        encoder->fillBuffer(dst, NS::Range(offset, bytes), byte);
        return;
    }
    // fillBuffer fills bytes only: other values are a pattern in command
    // memory, copied once, then doubled inside the range, so a range of any
    // size costs a few copies and one small allocation.
    const uint64 patternSize = std::min(bytes, kClearPatternSize);
    const CommandMemoryAllocation pattern = allocateCommandMemory(*cmd.internal.device, list, patternSize, 16);
    for (uint64 at = 0; at < patternSize; at += 4) {
        std::memcpy(pattern.cpu + at, &value, std::min<uint64>(4, patternSize - at));
    }
    encoder->copyFromBuffer(pattern.buffer, pattern.offset, dst, offset, patternSize);
    for (uint64 done = patternSize; done < bytes;) {
        const uint64 step = std::min(done, bytes - done);
        // Each copy reads what the previous one wrote.
        encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice);
        encoder->copyFromBuffer(dst, offset, dst, offset + done, step);
        done += step;
    }
}

void cmdWriteSmallData(CommandList &cmd, const Buffer &buffer, uint64 offset, std::span<const std::byte> data) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdWriteSmallData"));
    EQ_VALIDATE(Validation::checkSmallWrite((buffer.usage & BufferUsage::eCopyDst) != BufferUsage::eNone, offset,
                                            data.size(), buffer.size));
    // At most kMaxSmallWriteSize bytes, as D3D12 writes at most that many values.
    const uint64 size = std::min<uint64>(data.size(), kMaxSmallWriteSize) & ~uint64{3};
    if (size == 0) {
        return;
    }
    const CommandMemoryAllocation memory = allocateCommandMemory(*cmd.internal.device, list, size, 16);
    std::memcpy(memory.cpu, data.data(), size);
    computeEncoder(list)->copyFromBuffer(memory.buffer, memory.offset, static_cast<MTL::Buffer *>(buffer.internal.buffer),
                                         offset, size);
}

// ── Submission (P20) ─────────────────────────────────────────────────────────

TimelinePoint submit(Queue &queue, CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(queue.internal.device);
    DeviceState &state = *queue.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    CommandListState &list = *cmd.internal.state;
    if (list.recording) {
        reportError(ErrorCategory::eProgrammerError, "submit: the command list is still recording: call cmdEnd first");
    }
    // Metal asserts on a command buffer never begun or committed twice (P34:
    // a recording is submitted once).
    if (!list.recorded) {
        reportError(ErrorCategory::eProgrammerError,
                    "submit: the command list was not recorded since its last submission (cmdBegin ... cmdEnd, P34)");
    }
    // Heaps and command memory added since the last submission become resident.
    if (state.residencyDirty) {
        state.residency->commit();
        state.residencyDirty = false;
    }
    ScopedPool scope;
    MTL4::CommitOptions *options = MTL4::CommitOptions::alloc()->init();
    const uint64 value = state.lastSignaled + 1;
    // Runs on a Metal thread once the GPU is done, after the event is
    // signaled; wait and isReached wait for it and report the error.
    options->addFeedbackHandler([feedback = state.feedback, value](MTL4::CommitFeedback *result) {
        NS::Error *error = result->error();
        const NS::String *description = error != nullptr ? error->localizedDescription() : nullptr;
        {
            const std::lock_guard lock(feedback->mutex);
            if (error != nullptr && feedback->error.empty()) {
                feedback->error = description != nullptr ? description->utf8String() : "no error description";
                feedback->hasError.store(true, std::memory_order_release);
            }
            // In order on one queue in practice; `early` keeps `through` exact otherwise.
            feedback->early.insert(value);
            while (!feedback->early.empty() && *feedback->early.begin() == feedback->through + 1) {
                feedback->through = *feedback->early.begin();
                feedback->early.erase(feedback->early.begin());
            }
        }
        feedback->arrived.notify_all();
    });
    const MTL4::CommandBuffer *buffers[] = {list.commandBuffer};
    state.queue->commit(buffers, 1, options);
    options->release();
    state.queue->signalEvent(state.timeline, value);
    state.lastSignaled = value;
    cmd.internal.lastSubmitted = value;
    list.recorded = false;
    return {value};
}

// Any thread (P106): reads the event and the GpuFeedback only.
void wait(Queue &queue, TimelinePoint point) {
    DeviceState &state = *queue.internal.device;
    GpuFeedback &feedback = *state.feedback;
    {
        // The submission's feedback says whether it failed; a GPU error in any
        // submission ends the wait, as the device is lost.
        std::unique_lock lock(feedback.mutex);
        feedback.arrived.wait(lock, [&] {
            return feedback.through >= point.value || feedback.hasError.load(std::memory_order_relaxed);
        });
    }
    checkGpuError(state);
    // Signaled before the feedback in practice; waited for so isReached and
    // the idle checks agree once wait returns.
    while (!state.timeline->waitUntilSignaledValue(point.value, kWaitSliceMs)) {
        checkGpuError(state);
    }
}

bool isReached(Queue &queue, TimelinePoint point) {
    DeviceState &state = *queue.internal.device;
    const bool reached = completedValue(state) >= point.value;
    checkGpuError(state);
    return reached;
}

// ── Native access (P84) ──────────────────────────────────────────────────────

MTL4::CommandBuffer *native(const CommandList &cmd) { return cmd.internal.state->commandBuffer; }

} // namespace RI::Metal
