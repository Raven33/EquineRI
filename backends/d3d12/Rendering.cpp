// Rendering sections, draws, discard, clears and timestamps (P40, P51, P65,
// P68, P69, P71, P75, P95). D3D12 always uses A' with resting layouts: RI
// switches attachments into their attachment layout at cmdBeginRendering and
// back at cmdEndRendering. The half on RI's side is exact, the other full (P68).

#include "Barriers.hpp"
#include "D3D12Check.hpp"
#include "DeviceState.hpp"
#include "Formats.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <format>

namespace RI::D3D12 {

namespace {

ID3D12GraphicsCommandList7 *list(const CommandList &cmd) {
    return static_cast<ID3D12GraphicsCommandList7 *>(cmd.internal.list);
}

D3D12_BARRIER_LAYOUT resting(const TextureView &view) {
    return static_cast<D3D12_BARRIER_LAYOUT>(view.internal.restingLayout);
}

// Into an RI operation: wait for all earlier work (full), block exactly the
// operation (exact). `discardContents`: the old contents are not needed.
void enter(ID3D12GraphicsCommandList7 *l, const TextureView &view, D3D12_BARRIER_LAYOUT layout,
           D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access, bool discardContents) {
    if (!discardContents && resting(view) == layout) {
        return;
    }
    textureBarrier(l, view,
                   {.before = discardContents ? D3D12_BARRIER_LAYOUT_UNDEFINED : resting(view),
                    .after = layout,
                    .syncBefore = D3D12_BARRIER_SYNC_ALL,
                    .syncAfter = sync,
                    .accessBefore = discardContents ? D3D12_BARRIER_ACCESS_NO_ACCESS : D3D12_BARRIER_ACCESS_COMMON,
                    .accessAfter = access,
                    .discard = discardContents});
}

// Back to rest after an RI operation: wait exactly for it, block all later work.
void leave(ID3D12GraphicsCommandList7 *l, const TextureView &view, D3D12_BARRIER_LAYOUT layout,
           D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access) {
    if (resting(view) == layout) {
        return;
    }
    textureBarrier(l, view,
                   {.before = layout,
                    .after = resting(view),
                    .syncBefore = sync,
                    .syncAfter = D3D12_BARRIER_SYNC_ALL,
                    .accessBefore = access,
                    .accessAfter = D3D12_BARRIER_ACCESS_COMMON});
}

D3D12_CPU_DESCRIPTOR_HANDLE handle(uint64 ptr) { return {static_cast<SIZE_T>(ptr)}; }

bool depthReadOnlyLayout(const DepthAttachment &depth) {
    return depth.depthReadOnly && (!hasStencil(depth.view.format) || depth.stencilReadOnly);
}

// After a UAV clear in RI's own heap: the user's heaps and root signatures
// come back (P65 (3)), so user code continues as if nothing happened.
void restoreUserState(CommandList &cmd) {
    ID3D12GraphicsCommandList7 *l = list(cmd);
    if (cmd.internal.heapSet) {
        ID3D12DescriptorHeap *heaps[] = {static_cast<ID3D12DescriptorHeap *>(cmd.internal.heaps[0]),
                                         static_cast<ID3D12DescriptorHeap *>(cmd.internal.heaps[1])};
        l->SetDescriptorHeaps(2, heaps);
    }
    if (cmd.internal.rootSignature[0] != nullptr) {
        l->SetComputeRootSignature(static_cast<ID3D12RootSignature *>(cmd.internal.rootSignature[0]));
    }
    if (cmd.internal.rootSignature[1] != nullptr) {
        l->SetGraphicsRootSignature(static_cast<ID3D12RootSignature *>(cmd.internal.rootSignature[1]));
    }
}

void checkNotRendering(const CommandList &cmd, const char *call) {
    if (cmd.internal.rendering) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: not allowed inside a rendering section", call));
    }
}

} // namespace

void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc) {
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    checkNotRendering(cmd, "cmdBeginRendering");
    if (desc.colors.size() > 8) {
        reportError(ErrorCategory::eProgrammerError, "cmdBeginRendering: at most 8 color attachments");
    }
    ID3D12GraphicsCommandList7 *l = list(cmd);
    uint32 width = ~0u;
    uint32 height = ~0u;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8] = {};
    for (size_t i = 0; i < desc.colors.size(); ++i) {
        const ColorAttachment &a = desc.colors[i];
#if EQUINE_VALIDATION
        EQ_VALIDATE(Validation::checkDiscarded(state.placements, a.view.internal.textureId, "cmdBeginRendering"));
#endif
        if (a.view.internal.rtv == 0) {
            reportError(ErrorCategory::eProgrammerError,
                        "cmdBeginRendering: a color attachment needs a texture with TextureUsage::eRenderTarget");
        }
        enter(l, a.view, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
              D3D12_BARRIER_ACCESS_RENDER_TARGET, a.load != LoadOp::eLoad);
        rtvs[i] = handle(a.view.internal.rtv);
        if (a.load == LoadOp::eClear) {
            l->ClearRenderTargetView(rtvs[i], a.clear.color.data(), 0, nullptr);
        }
        cmd.internal.colors[i] = a.view;
        width = std::min(width, a.view.width);
        height = std::min(height, a.view.height);
    }
    cmd.internal.colorCount = static_cast<uint32>(desc.colors.size());

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    cmd.internal.depthReadOnly = false;
    cmd.internal.depth = {};
    if (desc.depth) {
        const DepthAttachment &d = *desc.depth;
#if EQUINE_VALIDATION
        EQ_VALIDATE(Validation::checkDiscarded(state.placements, d.view.internal.textureId, "cmdBeginRendering"));
#endif
        if (d.view.internal.dsv == 0) {
            reportError(ErrorCategory::eProgrammerError,
                        "cmdBeginRendering: a depth attachment needs a texture with TextureUsage::eDepthStencil");
        }
        const bool readOnly = depthReadOnlyLayout(d);
        const D3D12_BARRIER_LAYOUT layout =
            readOnly ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ : D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        const D3D12_BARRIER_ACCESS access =
            readOnly ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ : D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        const bool stencil = hasStencil(d.view.format);
        const bool discard = d.load != LoadOp::eLoad && (!stencil || d.stencilLoad != LoadOp::eLoad);
        enter(l, d.view, layout, D3D12_BARRIER_SYNC_DEPTH_STENCIL, access, discard && !readOnly);
        dsv = handle(readOnly ? d.view.internal.dsvReadOnly : d.view.internal.dsv);
        D3D12_CLEAR_FLAGS clear{};
        if (d.load == LoadOp::eClear) {
            clear |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if (stencil && d.stencilLoad == LoadOp::eClear) {
            clear |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if (clear != 0 && !readOnly) {
            l->ClearDepthStencilView(dsv, clear, d.clear.depth, d.clear.stencil, 0, nullptr);
        }
        cmd.internal.depth = d.view;
        cmd.internal.depthLayout = static_cast<uint32>(layout);
        cmd.internal.depthReadOnly = d.depthReadOnly;
        width = std::min(width, d.view.width);
        height = std::min(height, d.view.height);
    }

    l->OMSetRenderTargets(static_cast<UINT>(desc.colors.size()), desc.colors.empty() ? nullptr : rtvs, FALSE,
                          desc.depth ? &dsv : nullptr);
    if (width != ~0u) {
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        l->RSSetViewports(1, &viewport);
        l->RSSetScissorRects(1, &scissor);
    }
    cmd.internal.rendering = true;
}

void cmdEndRendering(CommandList &cmd) {
    if (!cmd.internal.rendering) {
        reportError(ErrorCategory::eProgrammerError, "cmdEndRendering: no rendering section is open");
    }
    ID3D12GraphicsCommandList7 *l = list(cmd);
    for (uint32 i = 0; i < cmd.internal.colorCount; ++i) {
        leave(l, cmd.internal.colors[i], D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
              D3D12_BARRIER_ACCESS_RENDER_TARGET);
        cmd.internal.colors[i] = {};
    }
    if (cmd.internal.depth.internal.resource != nullptr) {
        const auto layout = static_cast<D3D12_BARRIER_LAYOUT>(cmd.internal.depthLayout);
        leave(l, cmd.internal.depth, layout, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
              layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ
                                                                : D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        cmd.internal.depth = {};
    }
    cmd.internal.colorCount = 0;
    cmd.internal.rendering = false;
    cmd.internal.depthReadOnly = false;
}

void cmdDrawMesh(CommandList &cmd, uint32 x, uint32 y, uint32 z) { list(cmd)->DispatchMesh(x, y, z); }

void cmdSetIndexBuffer(CommandList &cmd, const Buffer &buffer, uint64 offset, IndexFormat format) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkIndexBuffer((buffer.usage & BufferUsage::eIndex) != BufferUsage::eNone, offset,
                                             format == IndexFormat::eUint16 ? 2 : 4, buffer.size));
    const D3D12_INDEX_BUFFER_VIEW view{
        .BufferLocation = buffer.gpuAddress + offset,
        .SizeInBytes = static_cast<UINT>(buffer.size - offset),
        .Format = format == IndexFormat::eUint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
    };
    list(cmd)->IASetIndexBuffer(&view);
}

void cmdDrawIndexed(CommandList &cmd, uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset,
                    uint32 firstInstance) {
    list(cmd)->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void cmdDraw(CommandList &cmd, uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) {
    list(cmd)->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

// Discard (P69): contents and compression metadata reset, full scope.
void cmdDiscard(CommandList &cmd, const TextureView &view) {
    checkNotRendering(cmd, "cmdDiscard");
#if EQUINE_VALIDATION
    Validation::markDiscarded(cmd.internal.device->placements, view.internal.textureId);
#endif
    textureBarrier(list(cmd), view,
                   {.before = D3D12_BARRIER_LAYOUT_UNDEFINED,
                    .after = resting(view),
                    .syncBefore = D3D12_BARRIER_SYNC_ALL,
                    .syncAfter = D3D12_BARRIER_SYNC_ALL,
                    .accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                    .accessAfter = D3D12_BARRIER_ACCESS_COMMON,
                    .discard = true});
}

// Clears (P65). Render targets and depth use their own clear calls inside an
// RI layout switch; other textures (storage) and buffers use a UAV clear with
// descriptors in RI's own shader-visible heap, then the user's heaps are set
// again (the user's heap holds no RI slots).
void cmdClearTexture(CommandList &cmd, const TextureView &view, const ClearValue &value) {
    DeviceState &state = *cmd.internal.device;
    checkNotRendering(cmd, "cmdClearTexture");
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, view.internal.textureId, "cmdClearTexture"));
#endif
    ID3D12GraphicsCommandList7 *l = list(cmd);
    if (view.internal.rtv != 0) {
        enter(l, view, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
              D3D12_BARRIER_ACCESS_RENDER_TARGET, true);
        l->ClearRenderTargetView(handle(view.internal.rtv), value.color.data(), 0, nullptr);
        leave(l, view, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
              D3D12_BARRIER_ACCESS_RENDER_TARGET);
        return;
    }
    if (view.internal.dsv != 0) {
        enter(l, view, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
              D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, true);
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (hasStencil(view.format)) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        l->ClearDepthStencilView(handle(view.internal.dsv), flags, value.depth, value.stencil, 0, nullptr);
        leave(l, view, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
              D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        return;
    }
    if (resting(view) != D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdClearTexture: the texture needs eRenderTarget, eDepthStencil or eStorage usage");
    }
    // A storage texture: UAV clear in place (DIRECT_QUEUE_COMMON allows UAV access).
    const uint32 slot = state.clearNext++ % kClearHeapSize;
    const UINT increment = state.resourceDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = state.clearCpuHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * increment;
    D3D12_CPU_DESCRIPTOR_HANDLE shaderCpu = state.clearHeap->GetCPUDescriptorHandleForHeapStart();
    shaderCpu.ptr += static_cast<SIZE_T>(slot) * increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = state.clearHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = static_cast<DXGI_FORMAT>(view.internal.dxgiFormat)};
    if (view.internal.textureLayers > 1) {
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray = {.MipSlice = view.internal.baseMip,
                              .FirstArraySlice = view.internal.baseLayer,
                              .ArraySize = view.internal.layerCount};
    } else {
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D = {.MipSlice = view.internal.baseMip};
    }
    auto *resource = static_cast<ID3D12Resource *>(view.internal.resource);
    state.device->CreateUnorderedAccessView(resource, nullptr, &uav, cpu);
    state.device->CreateUnorderedAccessView(resource, nullptr, &uav, shaderCpu);
    ID3D12DescriptorHeap *heaps[] = {state.clearHeap};
    l->SetDescriptorHeaps(1, heaps);
    l->ClearUnorderedAccessViewFloat(gpu, cpu, resource, value.color.data(), 0, nullptr);
    restoreUserState(cmd);
}

void cmdClearBuffer(CommandList &cmd, const Buffer &buffer, uint32 value) {
    DeviceState &state = *cmd.internal.device;
    checkNotRendering(cmd, "cmdClearBuffer");
    auto *resource = static_cast<ID3D12Resource *>(buffer.internal.resource);
    if ((resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdClearBuffer: the buffer needs BufferUsage::eCopyDst (or eReadWrite) and eGPU or eGPUUpload memory");
    }
    const uint32 slot = state.clearNext++ % kClearHeapSize;
    const UINT increment = state.resourceDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = state.clearCpuHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * increment;
    D3D12_CPU_DESCRIPTOR_HANDLE shaderCpu = state.clearHeap->GetCPUDescriptorHandleForHeapStart();
    shaderCpu.ptr += static_cast<SIZE_T>(slot) * increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = state.clearHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * increment;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = {.FirstElement = 0, .NumElements = static_cast<UINT>(buffer.size / 4), .Flags = D3D12_BUFFER_UAV_FLAG_RAW}};
    state.device->CreateUnorderedAccessView(resource, nullptr, &uav, cpu);
    state.device->CreateUnorderedAccessView(resource, nullptr, &uav, shaderCpu);
    ID3D12DescriptorHeap *heaps[] = {state.clearHeap};
    list(cmd)->SetDescriptorHeaps(1, heaps);
    const UINT values[4] = {value, value, value, value};
    list(cmd)->ClearUnorderedAccessViewUint(gpu, cpu, resource, values, 0, nullptr);
    restoreUserState(cmd);
}

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

namespace {

D3D12_QUERY_TYPE queryTypeOf(QueryType type) {
    switch (type) {
    case QueryType::eOcclusion: return D3D12_QUERY_TYPE_OCCLUSION;
    case QueryType::ePipelineStatistics: return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    default: return D3D12_QUERY_TYPE_TIMESTAMP;
    }
}

uint64 resultSizeOf(QueryType type) {
    return type == QueryType::ePipelineStatistics ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) : 8;
}

ID3D12QueryHeap *heapOf(const QueryPool &pool) { return static_cast<ID3D12QueryHeap *>(pool.internal.heap); }

} // namespace

void cmdWriteTimestamp(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type == QueryType::eTimestamp, "cmdWriteTimestamp"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdWriteTimestamp: the query"));
    list(cmd)->EndQuery(heapOf(pool), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void cmdBeginQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type != QueryType::eTimestamp, "cmdBeginQuery"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdBeginQuery: the query"));
    list(cmd)->BeginQuery(heapOf(pool), queryTypeOf(pool.type), index);
}

void cmdEndQuery(CommandList &cmd, const QueryPool &pool, uint32 index) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_VALIDATE(Validation::checkQueryKind(pool.type != QueryType::eTimestamp, "cmdEndQuery"));
    EQ_VALIDATE(Validation::checkRange(index, 1, pool.count, 1, "cmdEndQuery: the query"));
    list(cmd)->EndQuery(heapOf(pool), queryTypeOf(pool.type), index);
}

// D3D12 query heaps need no reset (P71 (1)).
void cmdResetQueries(CommandList &, const QueryPool &, uint32, uint32) {}

void cmdResolveQueries(CommandList &cmd, const QueryPool &pool, uint32 first, uint32 count, const Buffer &buffer,
                       uint64 offset) {
    EQ_DX_CHECK_ALIVE(cmd.internal.device, pool.internal.id, "query pool");
    EQ_DX_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdResolveQueries"));
    EQ_VALIDATE(Validation::checkRange(first, count, pool.count, 1, "cmdResolveQueries: the query range"));
    EQ_VALIDATE(Validation::checkRange(offset, uint64{count} * resultSizeOf(pool.type), buffer.size, 8,
                                       "cmdResolveQueries: the results in the buffer"));
    if ((buffer.usage & BufferUsage::eCopyDst) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError, "cmdResolveQueries: the buffer needs BufferUsage::eCopyDst");
    }
    list(cmd)->ResolveQueryData(heapOf(pool), queryTypeOf(pool.type), first, count,
                                static_cast<ID3D12Resource *>(buffer.internal.resource), offset);
}

// ── Copies (P47) ─────────────────────────────────────────────────────────────

namespace {

uint32 copyBytesOf(const TextureView &view) {
    if (view.internal.planes == 3) {
        return 0; // one aspect at a time
    }
    return copyBytesPerTexel(view.format, view.internal.planes == 2 ? Aspect::eStencil : Aspect::eDefault);
}

uint64 roundUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

void checkCopy(const TextureView &view, bool bufferCopy, TextureUsage usage, std::string_view usageName,
               std::string_view call) {
    EQ_VALIDATE(Validation::checkCopyView(view.internal.mipCount, view.samples, bufferCopy, copyBytesOf(view),
                                          (view.internal.textureUsage & usage) != TextureUsage::eNone, usageName,
                                          call));
}

ID3D12Resource *resourceOf(const TextureView &view) { return static_cast<ID3D12Resource *>(view.internal.resource); }

// The subresource of `layer` (counted from the view's first layer).
UINT subresourceOf(const TextureView &view, uint32 layer) {
    const UINT plane = view.internal.planes == 2 ? 1 : 0;
    return view.internal.baseMip + (view.internal.baseLayer + layer) * view.internal.textureMips +
           plane * view.internal.textureMips * view.internal.textureLayers;
}

// One layer's placed footprint at `offset`, as D3D12 describes it.
D3D12_TEXTURE_COPY_LOCATION bufferLocation(ID3D12Device *device, const TextureView &view, uint32 layer,
                                           ID3D12Resource *buffer, uint64 offset, const CopyLayout &layout) {
    const D3D12_RESOURCE_DESC desc = resourceOf(view)->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&desc, subresourceOf(view, layer), 1, 0, &footprint, nullptr, nullptr, nullptr);
    footprint.Offset = offset + layer * layout.layerPitch;
    footprint.Footprint.RowPitch = static_cast<UINT>(layout.rowPitch);
    D3D12_TEXTURE_COPY_LOCATION location{.pResource = buffer, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    location.PlacedFootprint = footprint;
    return location;
}

D3D12_TEXTURE_COPY_LOCATION textureLocation(const TextureView &view, uint32 layer) {
    D3D12_TEXTURE_COPY_LOCATION location{.pResource = resourceOf(view),
                                         .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    location.SubresourceIndex = subresourceOf(view, layer);
    return location;
}

void checkBufferCopyRange(const Buffer &buffer, uint64 offset, const CopyLayout &layout, BufferUsage usage,
                          std::string_view call) {
    EQ_VALIDATE(Validation::checkRange(offset, layout.size, buffer.size, layout.alignment,
                                       std::format("{}: the texels in the buffer", call)));
    if ((buffer.usage & usage) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the buffer needs BufferUsage::{}", call,
                                usage == BufferUsage::eCopySrc ? "eCopySrc" : "eCopyDst"));
    }
}

} // namespace

// Rows on D3D12's 256-byte pitch, layers on its 512-byte placement alignment.
CopyLayout copyLayout(Device & /*device*/, const TextureView &view) {
    CopyLayout layout;
    layout.width = view.width;
    layout.height = view.height;
    layout.layers = view.internal.layerCount;
    layout.bytesPerTexel = copyBytesOf(view);
    if (layout.bytesPerTexel == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    "copyLayout: the view has both depth and stencil; copies take one aspect (P47)");
    }
    layout.alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    layout.rowPitch = roundUp(uint64{layout.width} * layout.bytesPerTexel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64 layerBytes = layout.rowPitch * layout.height;
    layout.layerPitch = roundUp(layerBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    layout.size = layout.layerPitch * (layout.layers - 1) + layerBytes;
    return layout;
}

// Textures with copy usage rest in DIRECT_QUEUE_COMMON (L1), which allows
// copies: no layout switch.
void cmdCopyBufferToTexture(CommandList &cmd, const Buffer &src, uint64 srcOffset, const TextureView &dst) {
    DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, src.internal.id, "buffer");
    EQ_DX_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyBufferToTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyBufferToTexture"));
#endif
    checkCopy(dst, true, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyBufferToTexture");
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, dst);
    checkBufferCopyRange(src, srcOffset, layout, BufferUsage::eCopySrc, "cmdCopyBufferToTexture");
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        const D3D12_TEXTURE_COPY_LOCATION to = textureLocation(dst, layer);
        const D3D12_TEXTURE_COPY_LOCATION from =
            bufferLocation(state.device, dst, layer, static_cast<ID3D12Resource *>(src.internal.resource), srcOffset,
                           layout);
        list(cmd)->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
}

void cmdCopyTextureToBuffer(CommandList &cmd, const TextureView &src, const Buffer &dst, uint64 dstOffset) {
    DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_DX_CHECK_ALIVE(&state, dst.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyTextureToBuffer"));
    checkCopy(src, true, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTextureToBuffer");
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, src);
    checkBufferCopyRange(dst, dstOffset, layout, BufferUsage::eCopyDst, "cmdCopyTextureToBuffer");
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        const D3D12_TEXTURE_COPY_LOCATION to =
            bufferLocation(state.device, src, layer, static_cast<ID3D12Resource *>(dst.internal.resource), dstOffset,
                           layout);
        const D3D12_TEXTURE_COPY_LOCATION from = textureLocation(src, layer);
        list(cmd)->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
}

void cmdCopyTexture(CommandList &cmd, const TextureView &src, const TextureView &dst) {
    DeviceState &state = *cmd.internal.device;
    EQ_DX_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_DX_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.rendering, false, "cmdCopyTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyTexture"));
#endif
    checkCopy(src, false, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTexture");
    checkCopy(dst, false, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyTexture");
    EQ_VALIDATE(Validation::checkCopyTextureMatch(src.width, src.height, src.internal.layerCount, src.samples,
                                                  copyBytesOf(src), dst.width, dst.height, dst.internal.layerCount,
                                                  dst.samples, copyBytesOf(dst)));
    for (uint32 layer = 0; layer < src.internal.layerCount; ++layer) {
        const D3D12_TEXTURE_COPY_LOCATION to = textureLocation(dst, layer);
        const D3D12_TEXTURE_COPY_LOCATION from = textureLocation(src, layer);
        list(cmd)->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
}

} // namespace RI::D3D12
