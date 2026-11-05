// Textures, views and query pools on D3D12 (P51, P58, P69, P71, P73, P74, P95, P102).
//
// Owns: format support, texture requirements and placed textures, the resting
// layout of each texture (COMMON, or the attachment layout for attachment-only
// textures), views with RTV/DSV descriptors in RI's CPU heaps, query pools,
// and import of native textures (P77).
//
// Watch out: a new texture is reset at once on the internal list (R16, P69).
// Sampled depth textures are typeless (Formats.hpp). Cubes are 2D arrays until
// a shader view makes them cubes; a 3D texture has one subresource per mip.
//
// Vulkan counterpart: backends/vulkan/Textures.cpp and Resources.cpp.

#include "Barriers.hpp"
#include "D3D12Check.hpp"
#include "DeviceState.hpp"
#include "Formats.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

#include <algorithm>
#include <cstddef>
#include <bit>
#include <format>
#include <vector>

namespace RI::inline EQUINE_RI_ABI::D3D12 {

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
    // Cubes are 2D arrays; their shader views make them cubes (P102).
    const bool volume = desc.shape == TextureShape::e3D;
    return {
        .Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = desc.width,
        .Height = desc.height,
        .DepthOrArraySize = static_cast<UINT16>(volume ? desc.depth : desc.layers),
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
    // A 3D texture's layers are the depth slices of the view's first mip.
    const bool volume = texture.shape == TextureShape::e3D;
    const uint32 layers = volume ? std::max(1u, texture.depth >> std::min(baseMip, 31u)) : texture.layers;
    const uint32 baseLayer = desc.baseLayer;
    const uint32 layerCount = desc.layerCount == kAllLayers ? layers - baseLayer : desc.layerCount;
    if (volume) {
        EQ_VALIDATE(Validation::checkVolumeView(mipCount, baseLayer != 0 || layerCount != layers));
    }
    view.width = std::max(1u, texture.width >> baseMip);
    view.height = std::max(1u, texture.height >> baseMip);

    auto &in = view.internal;
    in.resource = texture.internal.resource;
    in.baseMip = baseMip;
    in.mipCount = mipCount;
    in.baseLayer = baseLayer;
    in.layerCount = layerCount;
    in.textureMips = texture.mips;
    in.textureLayers = volume ? 1 : texture.layers; // subresources: a 3D texture has one per mip
    in.shape = texture.shape;
    in.cube = texture.shape == TextureShape::eCube && baseLayer % 6 == 0 && layerCount % 6 == 0 && layerCount > 0;
    in.restingLayout = texture.internal.restingLayout;
    in.textureUsage = texture.usage;
    in.textureId = texture.internal.id;
    in.planes = planeMask(format, desc.aspect);
    in.dxgiFormat = isDepthFormat(format) ? depthSrvFormat(format, desc.aspect == Aspect::eStencil) : dxgiFormat(format);

    auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
    const bool multisampled = texture.samples > 1;
    const bool array = texture.layers > 1;
    if (has(texture.usage, TextureUsage::eRenderTarget) && !isDepthFormat(format)) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv{.Format = dxgiFormat(format)};
        if (volume) {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            rtv.Texture3D = {.MipSlice = baseMip, .FirstWSlice = baseLayer, .WSize = layerCount};
        } else if (multisampled) {
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

TextureView makeTextureView(DeviceState &state, const Texture &texture, const TextureViewDesc &desc) {
    return makeView(state, texture, desc);
}
void releaseTextureView(DeviceState &state, TextureView &view) { releaseViewSlots(state, view); }

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
        // A 3D texture's slices are not array slices: its range is slice 0.
        .Subresources = {.IndexOrFirstMipLevel = view.internal.baseMip,
                         .NumMipLevels = view.internal.mipCount,
                         .FirstArraySlice = view.internal.shape == TextureShape::e3D ? 0 : view.internal.baseLayer,
                         .NumArraySlices = view.internal.shape == TextureShape::e3D ? 1 : view.internal.layerCount,
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
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VALIDATE(Validation::checkTextureUsage(desc.format, desc.usage));
    EQ_VALIDATE(Validation::checkTextureShape(desc));
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
    texture.shape = desc.shape;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth = desc.depth;
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
    if (!desc.name.empty()) {
        applyName(*device.internal.state, texture, desc.name);
    }
    return texture;
}

void destroyTexture(Device &device, Texture &texture) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, texture.internal.id, "texture");
    EQ_DX_UNTRACK(&state, texture.internal.id);
    releaseViewSlots(state, texture.view);
    if (!texture.internal.imported) { // P77: an imported resource stays the user's
        auto *resource = static_cast<ID3D12Resource *>(texture.internal.resource);
        release(resource);
    }
    texture = {};
}

TextureView createView(Device &device, const Texture &texture, const TextureViewDesc &desc) {
    EQ_DX_CHECK_THREAD(device.internal.state);
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
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    releaseViewSlots(state, view);
    view = {};
}

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

QueryPool createQueryPool(Device &device, const QueryPoolDesc &desc) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    D3D12_QUERY_HEAP_TYPE type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (desc.type == QueryType::eOcclusion) {
        type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    } else if (desc.type == QueryType::ePipelineStatistics || desc.type == QueryType::eMeshPipelineStatistics) {
        type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    } else if (desc.type == QueryType::eMeshPrimitives) {
        if (!state.meshPrimitivesQuery) {
            reportError(ErrorCategory::eUnsupported,
                        "createQueryPool: this GPU cannot count mesh primitives (MeshShaderPipelineStatsSupported)");
        }
        type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1;
    }
    const D3D12_QUERY_HEAP_DESC heapDesc{.Type = type, .Count = std::max(desc.count, 1u)};
    ID3D12QueryHeap *heap = nullptr;
    checkHr(state.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
    QueryPool pool;
    pool.type = desc.type;
    pool.count = desc.count;
    pool.internal.device = &state;
    pool.internal.heap = heap;
    pool.internal.id = EQ_DX_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(*device.internal.state, pool, desc.name);
    }
    return pool;
}

void destroyQueryPool(Device &device, QueryPool &pool) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, pool.internal.id, "query pool");
    EQ_DX_UNTRACK(&state, pool.internal.id);
    auto *heap = static_cast<ID3D12QueryHeap *>(pool.internal.heap);
    release(heap);
    pool = {};
}

// D3D12 query heaps need no reset (P71 (1)).
void resetQueries([[maybe_unused]] Device &device, QueryPool &, uint32, uint32) {
    EQ_DX_CHECK_THREAD(device.internal.state);
}

// D3D12_QUERY_DATA_PIPELINE_STATISTICS(1), as D3D12 writes it (P71 (2)). The
// mesh set is the same native query with only its pixel and compute counters
// named; the primitive count is PIPELINE_STATISTICS1's MSPrimitives (P105).
PipelineStatisticsLayout pipelineStatisticsLayout(Device &device, QueryType type) {
    constexpr PipelineStatisticsLayout none{.inputVertices = kNoCounter,
                                            .inputPrimitives = kNoCounter,
                                            .vertexShaderInvocations = kNoCounter,
                                            .clippingInvocations = kNoCounter,
                                            .clippingPrimitives = kNoCounter,
                                            .pixelShaderInvocations = kNoCounter,
                                            .computeShaderInvocations = kNoCounter,
                                            .geometryShaderInvocations = kNoCounter,
                                            .geometryShaderPrimitives = kNoCounter,
                                            .hullShaderInvocations = kNoCounter,
                                            .domainShaderInvocations = kNoCounter};
    if (type == QueryType::eMeshPipelineStatistics) {
        PipelineStatisticsLayout layout = none;
        layout.size = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        layout.pixelShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, PSInvocations);
        layout.computeShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CSInvocations);
        return layout;
    }
    if (type == QueryType::eMeshPrimitives) {
        PipelineStatisticsLayout layout = none;
        layout.size = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
        layout.meshPrimitives = device.internal.state->meshPrimitivesQuery
                                    ? static_cast<uint32>(offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1, MSPrimitives))
                                    : kNoCounter;
        return layout;
    }
    if (type != QueryType::ePipelineStatistics) {
        PipelineStatisticsLayout layout = none;
        layout.size = 8;
        return layout;
    }
    return {.size = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
            .inputVertices = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, IAVertices),
            .inputPrimitives = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, IAPrimitives),
            .vertexShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, VSInvocations),
            .clippingInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CInvocations),
            .clippingPrimitives = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CPrimitives),
            .pixelShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, PSInvocations),
            .computeShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, CSInvocations),
            .geometryShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, GSInvocations),
            .geometryShaderPrimitives = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, GSPrimitives),
            .hullShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, HSInvocations),
            .domainShaderInvocations = offsetof(D3D12_QUERY_DATA_PIPELINE_STATISTICS, DSInvocations)};
}

// ── Import (P77) ─────────────────────────────────────────────────────────────

Texture importTexture(Device &device, ID3D12Resource *resource, const TextureDesc &desc) {
    EQ_DX_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_VALIDATE(Validation::checkTextureUsage(desc.format, desc.usage));
    EQ_VALIDATE(Validation::checkTextureShape(desc));
    Texture texture;
    texture.shape = desc.shape;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth = desc.depth;
    texture.layers = desc.layers;
    texture.mips = desc.mips;
    texture.samples = desc.samples;
    texture.format = desc.format;
    texture.usage = desc.usage;
    texture.internal.resource = resource;
    texture.internal.restingLayout = static_cast<uint32>(restingLayout(desc.usage));
    texture.internal.viewFormats = uint64{1} << static_cast<uint32>(desc.format);
    for (Format f : desc.viewFormats) {
        texture.internal.viewFormats |= uint64{1} << static_cast<uint32>(f);
    }
    texture.internal.imported = true;
    texture.internal.id = EQ_DX_TRACK(&state);
    texture.view = makeView(state, texture, {});
    return texture;
}

} // namespace RI::D3D12
