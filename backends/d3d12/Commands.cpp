// Command lists, binding, barriers and submission on D3D12 (P20, P34, P36,
// P56, P85).
//
// Owns: command lists (one allocator each), cmdBegin / cmdEnd, cmdSetHeap,
// cmdSetPipeline, cmdSetRootData, cmdBarrier (one global barrier from the
// stage masks), submit and wait (a fence as the timeline).
//
// Keeps: what is bound, in CommandList::internal, so RI can set the user's
// heaps and root signature again after its own clears and native sections.
//
// Watch out: heaps must be set before a root signature with the heap-indexing
// flags (P94 (10)). Attachment stages are left out of global barriers: RI's
// layout switches cover them (P95 (3)).
//
// Vulkan counterpart: backends/vulkan/Commands.cpp.

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace RI::D3D12 {

namespace {

ID3D12GraphicsCommandList7 *list(const CommandList &cmd) {
    return static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list);
}

D3D12_BARRIER_SYNC syncOf(Stage stage) {
    if ((stage & Stage::eAll) != Stage::eNone) {
        return D3D12_BARRIER_SYNC_ALL;
    }
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
    // Clears outside rendering sections count as copies (P65).
    if ((stage & Stage::eCopy) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
    }
    if ((stage & Stage::eCompute) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    }
    if ((stage & Stage::eIndirect) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    }
    if ((stage & Stage::eVertex) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    }
    if ((stage & Stage::ePixel) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    }
    if ((stage & Stage::eAccelerationStructure) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    // Attachment stages are not part of global barriers: under A' their writes
    // are made visible by RI's own switch back to rest at cmdEndRendering, and
    // D3D12 rejects sync bits that none of a global barrier's accesses use.
    return sync;
}

// The accesses a global barrier covers (coarse barriers, P36). Global
// barriers cannot touch layouts, so they list only what textures at rest (A')
// and buffers allow: shader reads and writes, copies, constant buffers,
// indirect arguments. Attachment accesses are made visible by RI's own switch
// back to rest at cmdEndRendering; textures resting in an attachment layout
// see only ordered attachment writes. An empty set is NO_ACCESS (an execution
// dependency), never COMMON.
// With ray tracing, shaders also read acceleration structures and builds
// write them (P49).
D3D12_BARRIER_ACCESS accessOf(Stage stage, bool rayTracing) {
    if ((stage & Stage::eAll) != Stage::eNone) {
        stage = Stage::eCopy | Stage::eCompute | Stage::eIndirect | Stage::eVertex | Stage::ePixel |
                (rayTracing ? Stage::eAccelerationStructure : Stage::eNone);
    }
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
    if ((stage & Stage::eCopy) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    }
    if ((stage & (Stage::eCompute | Stage::eVertex | Stage::ePixel)) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE |
                  D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    }
    if ((stage & Stage::eIndirect) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    }
    if (rayTracing && (stage & (Stage::eCompute | Stage::eVertex | Stage::ePixel)) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
    }
    if ((stage & Stage::eAccelerationStructure) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
                  D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
    }
    return access == D3D12_BARRIER_ACCESS_COMMON ? D3D12_BARRIER_ACCESS_NO_ACCESS : access;
}

uint64 completedValue(DeviceState &state) {
    const uint64 value = state.fence->GetCompletedValue();
    if (value == UINT64_MAX) {
        // A removed device reports UINT64_MAX on every fence.
        reportDeviceRemoved(state);
    }
    return value;
}

} // namespace

// ── Command lists (P56) ──────────────────────────────────────────────────────

CommandList createCommandList(Device &device, std::string_view name) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    ID3D12CommandAllocator *allocator = nullptr;
    checkHr(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "CreateCommandAllocator");
    // Created closed; cmdBegin opens it.
    ID3D12GraphicsCommandList7 *commandList = nullptr;
    checkHr(state.device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                             IID_PPV_ARGS(&commandList)),
            "CreateCommandList1");

    CommandList cmd;
    cmd.internal.device = &state;
    cmd.internal.list = commandList;
    cmd.internal.allocator = allocator;
    cmd.internal.id = EQ_DX_TRACK(&state);
    if (!name.empty()) {
        applyName(*device.internal.state, cmd, name);
    }
    return cmd;
}

void destroyCommandList(Device &device, CommandList &cmd) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "destroying"));
    EQ_DX_UNTRACK(&state, cmd.internal.id);
    auto *commandList = list(cmd);
    auto *allocator = static_cast<ID3D12CommandAllocator *>(cmd.internal.allocator);
    release(commandList);
    release(allocator);
    cmd = {};
}

void resetCommandList(CommandList &cmd) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "resetting"));
    checkHr(static_cast<ID3D12CommandAllocator *>(cmd.internal.allocator)->Reset(), "ID3D12CommandAllocator::Reset");
}

// ── Recording (P34: each call writes the native command at once) ─────────────

void cmdBegin(CommandList &cmd) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_DX_CHECK_ALIVE(cmd.internal.device, cmd.internal.id, "command list");
    checkHr(list(cmd)->Reset(static_cast<ID3D12CommandAllocator *>(cmd.internal.allocator), nullptr),
            "ID3D12GraphicsCommandList::Reset");
    cmd.internal.rootSignature[0] = nullptr;
    cmd.internal.rootSignature[1] = nullptr;
    cmd.internal.graphics = false;
    cmd.internal.heapSet = false;
    cmd.internal.markerDepth = 0;
    cmd.internal.openStatistics = 0;
    cmd.internal.pipelineState[0] = nullptr;
    cmd.internal.pipelineState[1] = nullptr;
    cmd.internal.rootDataBytes = 0;
    cmd.internal.native = false;
    cmd.internal.rootDataSize = 0;
    cmd.internal.rendering = false;
}

// Writes are visible to the CPU once the fence value is reached: no barrier needed.
void cmdEnd(CommandList &cmd) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkMarkersClosed(cmd.internal.markerDepth));
    checkHr(list(cmd)->Close(), "ID3D12GraphicsCommandList::Close");
}

void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_DX_CHECK_ALIVE(cmd.internal.device, heap.internal.id, "descriptor heap");
    ID3D12DescriptorHeap *heaps[] = {static_cast<ID3D12DescriptorHeap *>(heap.internal.resourceHeap),
                                     static_cast<ID3D12DescriptorHeap *>(heap.internal.samplerHeap)};
    list(cmd)->SetDescriptorHeaps(2, heaps);
    cmd.internal.heapSet = true;
    cmd.internal.heaps[0] = heaps[0];
    cmd.internal.heaps[1] = heaps[1];
    // Root signatures with heap-indexing flags read the heaps bound when they
    // are set, so bound root signatures are set again (P70: implicit state).
    if (cmd.internal.rootSignature[0] != nullptr) {
        list(cmd)->SetComputeRootSignature(static_cast<ID3D12RootSignature *>(cmd.internal.rootSignature[0]));
    }
    if (cmd.internal.rootSignature[1] != nullptr) {
        list(cmd)->SetGraphicsRootSignature(static_cast<ID3D12RootSignature *>(cmd.internal.rootSignature[1]));
    }
}

void cmdSetPipeline(CommandList &cmd, const Pipeline &pipeline) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_DX_CHECK_ALIVE(cmd.internal.device, pipeline.internal.id, "pipeline");
    EQ_VALIDATE(Validation::checkHeapSetBeforePipeline(cmd.internal.heapSet));
    EQ_VALIDATE(Validation::checkDepthWriteAllowed(cmd.internal.rendering && cmd.internal.depthReadOnly,
                                                   pipeline.internal.depthWrite));
    list(cmd)->SetPipelineState(static_cast<ID3D12PipelineState *>(pipeline.internal.pipelineState));
    const int point = pipeline.internal.graphics ? 1 : 0;
    if (cmd.internal.rootSignature[point] != pipeline.internal.rootSignature) {
        cmd.internal.rootSignature[point] = pipeline.internal.rootSignature;
        auto *rootSignature = static_cast<ID3D12RootSignature *>(pipeline.internal.rootSignature);
        if (pipeline.internal.graphics) {
            list(cmd)->SetGraphicsRootSignature(rootSignature);
        } else {
            list(cmd)->SetComputeRootSignature(rootSignature);
        }
    }
    if (pipeline.internal.graphics) {
        list(cmd)->IASetPrimitiveTopology(static_cast<D3D_PRIMITIVE_TOPOLOGY>(pipeline.internal.topology));
        // The pipeline's reference, unless cmdSetStencilReference set one in this section (P104).
        if (!cmd.internal.stencilOverride) {
            cmd.internal.stencilReference = pipeline.internal.stencilReference;
        }
        list(cmd)->OMSetStencilRef(cmd.internal.stencilReference);
    }
    cmd.internal.graphics = pipeline.internal.graphics;
    cmd.internal.rootDataSize = pipeline.rootDataSize;
    cmd.internal.pipelineState[point] = pipeline.internal.pipelineState;
    if (pipeline.internal.graphics) {
        cmd.internal.topology = pipeline.internal.topology;
    }
}

void cmdSetRootData(CommandList &cmd, std::span<const std::byte> data) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkRootDataSize(data.size(), cmd.internal.rootDataSize));
    const auto size = static_cast<uint32>(data.size());
    // Kept for endNative (P70).
    std::memcpy(cmd.internal.rootData, data.data(), std::min<size_t>(size, sizeof(cmd.internal.rootData)));
    cmd.internal.rootDataBytes = size;
    cmd.internal.rootDataGraphics = cmd.internal.graphics;
    // D3D12 reads whole 32-bit values: a span whose size is not a multiple of
    // 4 ends inside the last one, so the values come from the kept copy.
    const void *values = size <= sizeof(cmd.internal.rootData) ? static_cast<const void *>(cmd.internal.rootData)
                                                               : static_cast<const void *>(data.data());
    if (cmd.internal.graphics) {
        list(cmd)->SetGraphicsRoot32BitConstants(0, (size + 3) / 4, values, 0);
    } else {
        list(cmd)->SetComputeRoot32BitConstants(0, (size + 3) / 4, values, 0);
    }
}

void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkGroupCounts(x, y, z, false, "cmdDispatch"));
    list(cmd)->Dispatch(x, y, z);
}

// Coarse stage -> stage barrier (P36), as one global enhanced barrier.
void cmdBarrier(CommandList &cmd, Stage before, Stage after) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    if (((before | after) & Stage::eAccelerationStructure) != Stage::eNone && !cmd.internal.device->rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    "cmdBarrier: Stage::eAccelerationStructure needs ray tracing (DeviceLimits::rayTracing)");
    }
    if (syncOf(before) == D3D12_BARRIER_SYNC_NONE || syncOf(after) == D3D12_BARRIER_SYNC_NONE) {
        return; // only attachment stages on one side: RI's own transitions cover them
    }
    D3D12_GLOBAL_BARRIER barrier{
        .SyncBefore = syncOf(before),
        .SyncAfter = syncOf(after),
        .AccessBefore = accessOf(before, cmd.internal.device->rayTracing),
        .AccessAfter = accessOf(after, cmd.internal.device->rayTracing),
    };
    const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_GLOBAL, .NumBarriers = 1, .pGlobalBarriers = &barrier};
    list(cmd)->Barrier(1, &group);
}

void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size) {
    EQ_DX_CHECK_THREAD(cmd.internal.device);
    EQ_DX_CHECK_ALIVE(cmd.internal.device, src.internal.id, "buffer");
    EQ_DX_CHECK_ALIVE(cmd.internal.device, dst.internal.id, "buffer");
    list(cmd)->CopyBufferRegion(static_cast<ID3D12Resource *>(dst.internal.resource), dstOffset,
                                static_cast<ID3D12Resource *>(src.internal.resource), srcOffset, size);
}

// ── Submission (P20) ─────────────────────────────────────────────────────────

TimelinePoint submit(Queue &queue, CommandList &cmd) {
    EQ_DX_CHECK_THREAD(queue.internal.device);
    DeviceState &state = *queue.internal.device;
    EQ_DX_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    ID3D12CommandList *lists[] = {list(cmd)};
    state.graphicsQueue->ExecuteCommandLists(1, lists);
    const uint64 value = state.lastSignaled + 1;
    checkQueueHr(state, state.graphicsQueue->Signal(state.fence, value), "ID3D12CommandQueue::Signal");
    state.lastSignaled = value;
    cmd.internal.lastSubmitted = value;
    return {value};
}

void wait(Queue &queue, TimelinePoint point) {
    DeviceState &state = *queue.internal.device;
    if (completedValue(state) >= point.value) {
        return;
    }
    // A null event makes the call block until the fence reaches the value.
    checkQueueHr(state, state.fence->SetEventOnCompletion(point.value, nullptr), "ID3D12Fence::SetEventOnCompletion");
    completedValue(state);
}

bool isReached(Queue &queue, TimelinePoint point) { return completedValue(*queue.internal.device) >= point.value; }

} // namespace RI::D3D12
