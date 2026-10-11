// Textures, views and query pools (P51, P58, P69, P71, P73, P74, P95).

#include "Barriers.hpp"
#include "D3D12Check.hpp"
#include "DeviceState.hpp"
#include "Formats.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <vector>

namespace RI::D3D12 {

namespace {

bool has(TextureUsage usage, TextureUsage bit) { return (usage & bit) != TextureUsage::eNone; }

D3D12_RESOURCE_DESC1 textureDesc(const TextureDesc &desc) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (has(desc.usage, TextureUsage::eRenderTarget)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (has(desc.usage, TextureUsage::eDepthStencil)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!has(desc.usage, TextureUsage::eSampled)) {
            flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (has(desc.usage, TextureUsage::eStorage)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    // A sampled depth texture is typeless: its DSV and SRV use different formats.
    const bool typeless = isDepthFormat(desc.format) && has(desc.usage, TextureUsage::eSampled);
    return {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = desc.width,
        .Height = desc.height,
        .DepthOrArraySize = static_cast<UINT16>(desc.layers),
        .MipLevels = static_cast<UINT16>(desc.mips),
        .Format = typeless ? typelessDepthFormat(desc.format) : dxgiFormat(desc.format),
        .SampleDesc = {.Count = desc.samples, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = flags,
    };
}

uint32 planeMask(Format format, Aspect aspect) {
    if (!isDepthFormat(format)) {
        return 1;
    }
    switch (aspect) {
    case Aspect::eDepth: return 1;
    case Aspect::eStencil: return 2;
    default: return hasStencil(format) ? 3u : 1u;
    }
}

// Builds a view: facts, range, SRV format, and the RTV/DSV descriptors the
// texture's usage allows (P73: views are explicit; RI creates nothing else).
TextureView makeView(DeviceState &state, const Texture &texture, const TextureViewDesc &desc) {
    const Format format = desc.format == Format::eUndefined ? texture.format : desc.format;
    TextureView view;
    view.format = format;
    view.samples = texture.samples;
    const uint32 baseMip = desc.baseMip;
    const uint32 mipCount = desc.mipCount == kAllMips ? texture.mips - baseMip : desc.mipCount;
    const uint32 baseLayer = desc.baseLayer;
    const uint32 layerCount = desc.layerCount == kAllLayers ? texture.layers - baseLayer : desc.layerCount;
    view.width = std::max(1u, texture.width >> baseMip);
    view.height = std::max(1u, texture.height >> baseMip);

    auto &in = view.internal;
    in.resource = texture.internal.resource;
    in.baseMip = baseMip;
    in.mipCount = mipCount;
    in.baseLayer = baseLayer;
    in.layerCount = layerCount;
    in.textureMips = texture.mips;
    in.textureLayers = texture.layers;
    in.restingLayout = texture.internal.restingLayout;
    in.textureId = texture.internal.id;
    in.planes = planeMask(format, desc.aspect);
    in.dxgiFormat = isDepthFormat(format) ? depthSrvFormat(format, desc.aspect == Aspect::eStencil) : dxgiFormat(format);

    auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
    const bool multisampled = texture.samples > 1;
    const bool array = texture.layers > 1;
    if (has(texture.usage, TextureUsage::eRenderTarget) && !isDepthFormat(format)) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv{.Format = dxgiFormat(format)};
        if (multisampled) {
            rtv.ViewDimension = array ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
            rtv.Texture2DMSArray = {.FirstArraySlice = baseLayer, .ArraySize = layerCount};
        } else if (array) {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray = {.MipSlice = baseMip, .FirstArraySlice = baseLayer, .ArraySize = layerCount};
        } else {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtv.Texture2D = {.MipSlice = baseMip};
        }
        in.rtvSlot = allocateSlot(state.rtvHeap, "render-target view");
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = slotHandle(state.rtvHeap, in.rtvSlot);
        state.device->CreateRenderTargetView(resource, &rtv, handle);
        in.rtv = handle.ptr;
    }
    if (has(texture.usage, TextureUsage::eDepthStencil) && isDepthFormat(format)) {
        const auto makeDsv = [&](D3D12_DSV_FLAGS flags, uint32 &slot, uint64 &out) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{.Format = dxgiFormat(format), .Flags = flags};
            if (multisampled) {
                dsv.ViewDimension = array ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMS;
                dsv.Texture2DMSArray = {.FirstArraySlice = baseLayer, .ArraySize = layerCount};
            } else if (array) {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Texture2DArray = {.MipSlice = baseMip, .FirstArraySlice = baseLayer, .ArraySize = layerCount};
            } else {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsv.Texture2D = {.MipSlice = baseMip};
            }
            slot = allocateSlot(state.dsvHeap, "depth-stencil view");
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = slotHandle(state.dsvHeap, slot);
            state.device->CreateDepthStencilView(resource, &dsv, handle);
            out = handle.ptr;
        };
        makeDsv(D3D12_DSV_FLAG_NONE, in.dsvSlot, in.dsv);
        const D3D12_DSV_FLAGS readOnly =
            hasStencil(format) ? D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL
                               : D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        makeDsv(readOnly, in.dsvReadOnlySlot, in.dsvReadOnly);
    }
    return view;
}

void releaseViewSlots(DeviceState &state, TextureView &view) {
    if (view.internal.rtvSlot != ~0u) {
        state.rtvHeap.free.push_back(view.internal.rtvSlot);
    }
    if (view.internal.dsvSlot != ~0u) {
        state.dsvHeap.free.push_back(view.internal.dsvSlot);
    }
    if (view.internal.dsvReadOnlySlot != ~0u) {
        state.dsvHeap.free.push_back(view.internal.dsvReadOnlySlot);
    }
}

} // namespace

D3D12_BARRIER_LAYOUT restingLayout(TextureUsage usage) {
    if (has(usage, TextureUsage::eSampled) || has(usage, TextureUsage::eStorage) ||
        has(usage, TextureUsage::eCopySrc) || has(usage, TextureUsage::eCopyDst)) {
        return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON;
    }
    if (has(usage, TextureUsage::eRenderTarget)) {
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    }
    if (has(usage, TextureUsage::eDepthStencil)) {
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    }
    return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON;
}

void textureBarrier(ID3D12GraphicsCommandList7 *list, const TextureView &view, const TextureBarrier &b) {
    const uint32 planes = view.internal.planes;
    const D3D12_TEXTURE_BARRIER barrier{
        .SyncBefore = b.syncBefore,
        .SyncAfter = b.syncAfter,
        .AccessBefore = b.accessBefore,
        .AccessAfter = b.accessAfter,
        .LayoutBefore = b.before,
        .LayoutAfter = b.after,
        .pResource = static_cast<ID3D12Resource *>(view.internal.resource),
        .Subresources = {.IndexOrFirstMipLevel = view.internal.baseMip,
                         .NumMipLevels = view.internal.mipCount,
                         .FirstArraySlice = view.internal.baseLayer,
                         .NumArraySlices = view.internal.layerCount,
                         .FirstPlane = (planes & 1u) != 0 ? 0u : 1u,
                         .NumPlanes = static_cast<UINT>(std::popcount(planes))},
        .Flags = b.discard ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE,
    };
    const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &barrier};
    list->Barrier(1, &group);
}

// ── Formats (P58 (2)) ────────────────────────────────────────────────────────

bool formatSupported(Device &device, Format format, TextureUsage usage, uint32 samples) {
    DeviceState &state = *device.internal.state;
    const bool depth = isDepthFormat(format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = dxgiFormat(format)};
    if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT read = support;
    if (depth && has(usage, TextureUsage::eSampled)) {
        read.Format = depthSrvFormat(format, false);
        if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &read, sizeof(read)))) {
            return false;
        }
    }
    const auto need = [](bool want, bool have) { return !want || have; };
    bool ok = support.Support1 != D3D12_FORMAT_SUPPORT1_NONE;
    ok = ok && need(has(usage, TextureUsage::eSampled), (read.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0);
    ok = ok && need(has(usage, TextureUsage::eStorage),
                    (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
    ok = ok && need(has(usage, TextureUsage::eRenderTarget),
                    (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0);
    ok = ok && need(has(usage, TextureUsage::eDepthStencil),
                    (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0);
    if (ok && samples > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{.Format = dxgiFormat(format), .SampleCount = samples};
        ok = SUCCEEDED(state.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                         sizeof(levels))) &&
             levels.NumQualityLevels > 0;
    }
    return ok;
}

// ── Textures ─────────────────────────────────────────────────────────────────

MemoryRequirements textureRequirements(Device &device, const TextureDesc &desc) {
    DeviceState &state = *device.internal.state;
    const D3D12_RESOURCE_DESC1 native = textureDesc(desc);
    const D3D12_RESOURCE_ALLOCATION_INFO info = state.device->GetResourceAllocationInfo2(0, 1, &native, nullptr);
    // Textures live in GPU memory (D3D12 heaps for CPU access hold buffers only here).
    return {.size = info.SizeInBytes, .alignment = info.Alignment, .kinds = 1u << static_cast<uint32>(MemoryKind::eGPU)};
}

Texture createTexture(Device &device, const Memory &memory, uint64 offset, const TextureDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VALIDATE(Validation::checkTextureUsage(desc.format, desc.usage));
    const MemoryRequirements requirements = textureRequirements(device, desc);
    if (!fits(requirements, memory.kind)) {
        reportError(ErrorCategory::eUnsupported, "createTexture: textures need eGPU memory on D3D12");
    }
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, (memory.size + 65535) / 65536 * 65536,
                                       requirements.alignment, "createTexture: placing the texture in memory"));

    // Castable formats (P74): exact list, so drivers can keep compression.
    std::vector<DXGI_FORMAT> castable;
    for (Format f : desc.viewFormats) {
        castable.push_back(dxgiFormat(f));
    }
    const D3D12_RESOURCE_DESC1 native = textureDesc(desc);
    ID3D12Resource *resource = nullptr;
    checkHr(state.device->CreatePlacedResource2(static_cast<ID3D12Heap *>(memory.internal.heap), offset, &native,
                                                D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr,
                                                static_cast<UINT32>(castable.size()),
                                                castable.empty() ? nullptr : castable.data(), IID_PPV_ARGS(&resource)),
            "CreatePlacedResource2 (texture)");

    Texture texture;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.layers = desc.layers;
    texture.mips = desc.mips;
    texture.samples = desc.samples;
    texture.format = desc.format;
    texture.usage = desc.usage;
    texture.internal.resource = resource;
    texture.internal.restingLayout = restingLayout(desc.usage);
    texture.internal.viewFormats = uint64{1} << static_cast<uint32>(desc.format);
    for (Format f : desc.viewFormats) {
        texture.internal.viewFormats |= uint64{1} << static_cast<uint32>(f);
    }
    texture.internal.id = EQ_DX_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, texture.internal.id,
                              true);
#endif
    texture.view = makeView(state, texture, {});

    // The initial reset (R16, P69): contents undefined, metadata initialized.
    submitInternal(state, [&](ID3D12GraphicsCommandList7 *list) {
        textureBarrier(list, texture.view,
                       {.before = D3D12_BARRIER_LAYOUT_UNDEFINED,
                        .after = static_cast<D3D12_BARRIER_LAYOUT>(texture.internal.restingLayout),
                        .syncBefore = D3D12_BARRIER_SYNC_NONE,
                        .syncAfter = D3D12_BARRIER_SYNC_NONE,
                        .accessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS,
                        .accessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS,
                        .discard = true});
    });
    return texture;
}

void destroyTexture(Device &device, Texture &texture) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, texture.internal.id, "texture");
    EQ_DX_UNTRACK(&state, texture.internal.id);
    releaseViewSlots(state, texture.view);
    auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
    release(resource);
    texture = {};
}

TextureView createView(Device &device, const Texture &texture, const TextureViewDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, texture.internal.id, "texture");
    const Format format = desc.format == Format::eUndefined ? texture.format : desc.format;
    if ((texture.internal.viewFormats & (uint64{1} << static_cast<uint32>(format))) == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createView: format {} is not the texture's format or one of its viewFormats (P74)",
                                static_cast<uint32>(format)));
    }
    EQ_VALIDATE(bytesPerTexel(format) == bytesPerTexel(texture.format)
                    ? Validation::CheckResult{}
                    : Validation::CheckResult{"createView: a view format must have the texture's texel size (P74)"});
    return makeView(state, texture, desc);
}

void destroyView(Device &device, TextureView &view) {
    DeviceState &state = *device.internal.state;
    releaseViewSlots(state, view);
    view = {};
}

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc) {
    DeviceState &state = *device.internal.state;
    const D3D12_QUERY_HEAP_DESC heapDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = std::max(desc.count, 1u)};
    ID3D12QueryHeap *heap = nullptr;
    checkHr(state.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
    QueryPool pool;
    pool.type = desc.type;
    pool.count = desc.count;
    pool.internal.device = &state;
    pool.internal.heap = heap;
    pool.internal.id = EQ_DX_TRACK(&state);
    return pool;
}

void destroyQueryPool(Device &device, QueryPool &pool) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_DX_UNTRACK(&state, pool.internal.id);
    auto *heap = static_cast<ID3D12QueryHeap *>(pool.internal.heap);
    release(heap);
    pool = {};
}

// D3D12 query heaps need no reset (P71 (1)).
void resetQueries(Device &, QueryPool &, uint32, uint32) {}

} // namespace RI::D3D12
