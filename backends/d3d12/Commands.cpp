// Command lists, recording and submission (P20, P34, P36, P47, P56, P85).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

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
    if ((stage & Stage::eRenderTarget) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
    }
    if ((stage & Stage::eDepthStencil) != Stage::eNone) {
        sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    }
    return sync;
}

// Every access the stages can make (coarse barriers, P36).
D3D12_BARRIER_ACCESS accessOf(Stage stage) {
    // COMMON on one side only is invalid in a global barrier, so eAll lists every access.
    if ((stage & Stage::eAll) != Stage::eNone) {
        stage = Stage::eCopy | Stage::eCompute | Stage::eIndirect | Stage::eRenderTarget | Stage::eDepthStencil;
    }
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
    if ((stage & Stage::eCopy) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    }
    if ((stage & (Stage::eCompute | Stage::eVertex | Stage::ePixel)) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE |
                  D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    }
    if ((stage & Stage::eRenderTarget) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
    }
    if ((stage & Stage::eDepthStencil) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    }
    if ((stage & Stage::eIndirect) != Stage::eNone) {
        access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    }
    return access;
}

uint64 completedValue(DeviceState &state) {
    const uint64 value = state.fence->GetCompletedValue();
    if (value == UINT64_MAX) {
        // A removed device reports UINT64_MAX on every fence.
        checkHr(state.device->GetDeviceRemovedReason(), "the D3D12 device was removed");
    }
    return value;
}

} // namespace

// ── Command lists (P56) ──────────────────────────────────────────────────────

CommandList createCommandList(Device &device) {
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
    return cmd;
}

void destroyCommandList(Device &device, CommandList &cmd) {
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
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    EQ_VALIDATE(Validation::checkCommandListIdle(cmd.internal.lastSubmitted, completedValue(state), "resetting"));
    checkHr(static_cast<ID3D12CommandAllocator *>(cmd.internal.allocator)->Reset(), "ID3D12CommandAllocator::Reset");
}

// ── Recording (P34: each call writes the native command at once) ─────────────

void cmdBegin(CommandList &cmd) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, cmd.internal.id, "command list");
    checkHr(list(cmd)->Reset(static_cast<ID3D12CommandAllocator *>(cmd.internal.allocator), nullptr),
            "ID3D12GraphicsCommandList::Reset");
    cmd.internal.rootSignature[0] = nullptr;
    cmd.internal.rootSignature[1] = nullptr;
    cmd.internal.graphics = false;
    cmd.internal.heapSet = false;
    cmd.internal.rootDataSize = 0;
    cmd.internal.rendering = false;
}

// Writes are visible to the CPU once the fence value is reached: no barrier needed.
void cmdEnd(CommandList &cmd) { checkHr(list(cmd)->Close(), "ID3D12GraphicsCommandList::Close"); }

void cmdSetHeap(CommandList &cmd, const DescriptorHeap &heap) {
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
    }
    cmd.internal.graphics = pipeline.internal.graphics;
    cmd.internal.rootDataSize = pipeline.rootDataSize;
}

void cmdSetRootData(CommandList &cmd, const void *data, uint32 size) {
    EQ_VALIDATE(Validation::checkRootDataSize(size, cmd.internal.rootDataSize));
    if (cmd.internal.graphics) {
        list(cmd)->SetGraphicsRoot32BitConstants(0, (size + 3) / 4, data, 0);
    } else {
        list(cmd)->SetComputeRoot32BitConstants(0, (size + 3) / 4, data, 0);
    }
}

void cmdDispatch(CommandList &cmd, uint32 x, uint32 y, uint32 z) { list(cmd)->Dispatch(x, y, z); }

// Coarse stage -> stage barrier (P36), as one global enhanced barrier.
void cmdBarrier(CommandList &cmd, Stage before, Stage after) {
    D3D12_GLOBAL_BARRIER barrier{
        .SyncBefore = syncOf(before),
        .SyncAfter = syncOf(after),
        .AccessBefore = accessOf(before),
        .AccessAfter = accessOf(after),
    };
    const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_GLOBAL, .NumBarriers = 1, .pGlobalBarriers = &barrier};
    list(cmd)->Barrier(1, &group);
}

void cmdCopyBuffer(CommandList &cmd, const Buffer &src, uint64 srcOffset, const Buffer &dst, uint64 dstOffset,
                   uint64 size) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, src.internal.id, "buffer");
    EQ_DX_CHECK_ALIVE(cmd.internal.device, dst.internal.id, "buffer");
    list(cmd)->CopyBufferRegion(static_cast<ID3D12Resource *>(dst.internal.resource), dstOffset,
                                static_cast<ID3D12Resource *>(src.internal.resource), srcOffset, size);
}

// ── Submission (P20) ─────────────────────────────────────────────────────────

TimelinePoint submit(Queue &queue, CommandList &cmd) {
    DeviceState &state = *queue.internal.device;
    EQ_DX_CHECK_ALIVE(&state, cmd.internal.id, "command list");
    ID3D12CommandList *lists[] = {list(cmd)};
    state.graphicsQueue->ExecuteCommandLists(1, lists);
    const uint64 value = state.lastSignaled + 1;
    checkHr(state.graphicsQueue->Signal(state.fence, value), "ID3D12CommandQueue::Signal");
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
    checkHr(state.fence->SetEventOnCompletion(point.value, nullptr), "ID3D12Fence::SetEventOnCompletion");
    completedValue(state);
}

bool isReached(Queue &queue, TimelinePoint point) { return completedValue(*queue.internal.device) >= point.value; }

} // namespace RI::D3D12
