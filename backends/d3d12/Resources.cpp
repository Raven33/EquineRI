// Memory, buffers and descriptor heaps (P31, P32, P33, P58, P59, P61, P87, P94).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <type_traits>
#include <variant>

namespace RI::D3D12 {

namespace {

constexpr uint64 kPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64 KiB for buffers

const char *nameOf(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::eGPU: return "eGPU";
    case MemoryKind::eUpload: return "eUpload";
    case MemoryKind::eReadback: return "eReadback";
    case MemoryKind::eGPUUpload: return "eGPUUpload";
    }
    return "?";
}

D3D12_HEAP_TYPE heapTypeOf(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::eGPU: return D3D12_HEAP_TYPE_DEFAULT;
    case MemoryKind::eUpload: return D3D12_HEAP_TYPE_UPLOAD;
    case MemoryKind::eReadback: return D3D12_HEAP_TYPE_READBACK;
    case MemoryKind::eGPUUpload: return D3D12_HEAP_TYPE_GPU_UPLOAD;
    }
    return D3D12_HEAP_TYPE_DEFAULT;
}

// `clearable`: eCopyDst buffers in GPU memory also allow UAV access, which
// RI's cmdClearBuffer needs on D3D12 (P65); UPLOAD/READBACK heaps forbid it.
D3D12_RESOURCE_DESC1 bufferDesc(uint64 size, BufferUsage usage, bool clearable = false) {
    return {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = kPlacementAlignment,
        .Width = std::max<uint64>(size, 1),
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = (usage & BufferUsage::eReadWrite) != BufferUsage::eNone ||
                         (clearable && (usage & BufferUsage::eCopyDst) != BufferUsage::eNone)
                     ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                     : D3D12_RESOURCE_FLAG_NONE,
    };
}

// Buffers start, and stay, in the UNDEFINED layout (enhanced barriers).
ID3D12Resource *placeBuffer(DeviceState &state, ID3D12Heap *heap, uint64 offset, const D3D12_RESOURCE_DESC1 &desc) {
    ID3D12Resource *resource = nullptr;
    checkHr(state.device->CreatePlacedResource2(heap, offset, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0,
                                                nullptr, IID_PPV_ARGS(&resource)),
            "CreatePlacedResource2");
    return resource;
}

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// Texture views in descriptors (P87): an SRV or a UAV of the view's range.
// Returns false when the descriptor names a buffer.
bool writeTextureView(DeviceState &state, const Descriptor &descriptor, D3D12_CPU_DESCRIPTOR_HANDLE slot) {
    // Top-level acceleration structures (P49): an SRV naming the address.
    if (const auto *as = std::get_if<AccelerationStructureView>(&descriptor)) {
        EQ_DX_CHECK_ALIVE(&state, as->tlas.internal.id, "acceleration structure");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format = DXGI_FORMAT_UNKNOWN,
                                            .ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
                                            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        srv.RaytracingAccelerationStructure.Location = as->tlas.gpuAddress;
        state.device->CreateShaderResourceView(nullptr, &srv, slot);
        return true;
    }
    const TextureView *view = nullptr;
    bool readWrite = false;
    if (const auto *r = std::get_if<ReadView>(&descriptor); r != nullptr && r->texture.internal.resource != nullptr) {
        view = &r->texture;
    } else if (const auto *w = std::get_if<ReadWriteView>(&descriptor);
               w != nullptr && w->texture.internal.resource != nullptr) {
        view = &w->texture;
        readWrite = true;
    }
    if (view == nullptr) {
        return false;
    }
    EQ_DX_CHECK_ALIVE(&state, view->internal.textureId, "texture");
    const auto &in = view->internal;
    auto *resource = static_cast<ID3D12Resource *>(in.resource);
    const bool array = in.textureLayers > 1;
    const UINT planeSlice = (in.planes & 1u) != 0 ? 0u : 1u;
    if (readWrite) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = static_cast<DXGI_FORMAT>(in.dxgiFormat)};
        if (array) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray = {.MipSlice = in.baseMip, .FirstArraySlice = in.baseLayer, .ArraySize = in.layerCount};
        } else {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D = {.MipSlice = in.baseMip};
        }
        state.device->CreateUnorderedAccessView(resource, nullptr, &uav, slot);
        return true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format = static_cast<DXGI_FORMAT>(in.dxgiFormat),
                                        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    if (view->samples > 1) {
        srv.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Texture2DMSArray = {.FirstArraySlice = in.baseLayer, .ArraySize = in.layerCount};
    } else if (array) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray = {.MostDetailedMip = in.baseMip,
                              .MipLevels = in.mipCount,
                              .FirstArraySlice = in.baseLayer,
                              .ArraySize = in.layerCount,
                              .PlaneSlice = planeSlice};
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D = {.MostDetailedMip = in.baseMip, .MipLevels = in.mipCount, .PlaneSlice = planeSlice};
    }
    state.device->CreateShaderResourceView(resource, &srv, slot);
    return true;
}

} // namespace

// ── Memory ───────────────────────────────────────────────────────────────────

// D3D12 has one heap type per kind, and heap tier 2 holds every resource:
// `forResource` changes nothing.
Memory allocateMemory(Device &device, MemoryKind kind, uint64 size, const MemoryRequirements *) {
    DeviceState &state = *device.internal.state;
    if (kind == MemoryKind::eGPUUpload && !state.gpuUploadHeap) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("memory kind {} is not available on '{}' (see DeviceLimits::gpuUploadMemory)",
                                nameOf(kind), device.gpu.name));
    }
    // GPU memory may hold MSAA textures, which need 4 MiB placement alignment.
    const uint64 heapAlignment =
        kind == MemoryKind::eGPU ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : kPlacementAlignment;
    const uint64 heapSize = alignUp(std::max<uint64>(size, 1), heapAlignment);
    const D3D12_HEAP_DESC desc{
        .SizeInBytes = heapSize,
        .Properties = {.Type = heapTypeOf(kind)},
        .Alignment = heapAlignment,
        .Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES, // resource heap tier 2
    };
    ID3D12Heap *heap = nullptr;
    checkHr(state.device->CreateHeap(&desc, IID_PPV_ARGS(&heap)), "CreateHeap");

    Memory result;
    result.kind = kind;
    result.size = size;
    // CPU-visible kinds stay mapped for their lifetime (P33). D3D12 maps
    // resources, not heaps, so RI maps one buffer spanning the whole heap;
    // buffers placed later share its memory.
    if (kind != MemoryKind::eGPU) {
        ID3D12Resource *whole = placeBuffer(state, heap, 0, bufferDesc(heapSize, BufferUsage::eNone));
        const D3D12_RANGE readRange{0, kind == MemoryKind::eReadback ? static_cast<SIZE_T>(heapSize) : 0};
        checkHr(whole->Map(0, &readRange, &result.mapped), "ID3D12Resource::Map");
        result.internal.mapBuffer = whole;
    }
    result.internal.heap = heap;
    result.internal.id = EQ_DX_TRACK(&state);
    return result;
}

void freeMemory(Device &device, Memory &memory) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_DX_UNTRACK(&state, memory.internal.id);
#if EQUINE_VALIDATION
    Validation::forgetMemory(state.placements, memory.internal.id);
#endif
    if (!memory.internal.imported) { // P77: an imported heap stays the user's
        auto *whole = static_cast<ID3D12Resource *>(memory.internal.mapBuffer);
        release(whole);
        auto *heap = static_cast<ID3D12Heap *>(memory.internal.heap);
        release(heap);
    }
    memory = {};
}

// ── Buffers ──────────────────────────────────────────────────────────────────

MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    const D3D12_RESOURCE_DESC1 native = bufferDesc(desc.size, desc.usage);
    const D3D12_RESOURCE_ALLOCATION_INFO info = state.device->GetResourceAllocationInfo2(0, 1, &native, nullptr);
    uint32 kinds = 0;
    for (MemoryKind kind : {MemoryKind::eGPU, MemoryKind::eUpload, MemoryKind::eReadback}) {
        kinds |= 1u << static_cast<uint32>(kind);
    }
    if (state.gpuUploadHeap) {
        kinds |= 1u << static_cast<uint32>(MemoryKind::eGPUUpload);
    }
    return {.size = info.SizeInBytes, .alignment = info.Alignment, .kinds = kinds};
}

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    [[maybe_unused]] const MemoryRequirements requirements = bufferRequirements(device, desc);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, alignUp(memory.size, kPlacementAlignment),
                                       requirements.alignment, "createBuffer: placing the buffer in memory"));

    const bool clearable = memory.kind == MemoryKind::eGPU || memory.kind == MemoryKind::eGPUUpload;
    ID3D12Resource *resource = placeBuffer(state, static_cast<ID3D12Heap *>(memory.internal.heap), offset,
                                           bufferDesc(desc.size, desc.usage, clearable));
    Buffer result;
    result.size = desc.size;
    result.usage = desc.usage;
    result.gpuAddress = resource->GetGPUVirtualAddress();
    result.mapped = memory.mapped != nullptr ? static_cast<std::byte *>(memory.mapped) + offset : nullptr;
    result.internal.resource = resource;
    result.internal.id = EQ_DX_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, result.internal.id,
                              false);
#endif
    return result;
}

void destroyBuffer(Device &device, Buffer &buffer) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
    EQ_DX_UNTRACK(&state, buffer.internal.id);
    if (!buffer.internal.imported) { // P77: an imported resource stays the user's
        auto *resource = static_cast<ID3D12Resource *>(buffer.internal.resource);
        release(resource);
    }
    buffer = {};
}

// ── Descriptor heaps (P94) ───────────────────────────────────────────────────

DescriptorHeap createDescriptorHeap(Device &device, const DescriptorHeapDesc &desc) {
    DeviceState &state = *device.internal.state;
    if (desc.resourceCount > device.limits.maxResourceDescriptors ||
        desc.samplerCount > device.limits.maxSamplerDescriptors) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createDescriptorHeap: {} resource / {} sampler slots exceed the device limits "
                                "({} / {}, Device::limits)",
                                desc.resourceCount, desc.samplerCount, device.limits.maxResourceDescriptors,
                                device.limits.maxSamplerDescriptors));
    }
    const D3D12_DESCRIPTOR_HEAP_DESC resourceDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = std::max(desc.resourceCount, 1u),
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    const D3D12_DESCRIPTOR_HEAP_DESC samplerDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                 .NumDescriptors = std::max(desc.samplerCount, 1u),
                                                 .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap *resources = nullptr;
    ID3D12DescriptorHeap *samplers = nullptr;
    checkHr(state.device->CreateDescriptorHeap(&resourceDesc, IID_PPV_ARGS(&resources)), "CreateDescriptorHeap");
    checkHr(state.device->CreateDescriptorHeap(&samplerDesc, IID_PPV_ARGS(&samplers)), "CreateDescriptorHeap");

    DescriptorHeap heap;
    heap.resourceCount = desc.resourceCount;
    heap.samplerCount = desc.samplerCount;
    heap.internal.resourceHeap = resources;
    heap.internal.samplerHeap = samplers;
    heap.internal.id = EQ_DX_TRACK(&state);
    return heap;
}

void destroyDescriptorHeap(Device &device, DescriptorHeap &heap) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    EQ_DX_UNTRACK(&state, heap.internal.id);
    auto *resources = static_cast<ID3D12DescriptorHeap *>(heap.internal.resourceHeap);
    auto *samplers = static_cast<ID3D12DescriptorHeap *>(heap.internal.samplerHeap);
    release(resources);
    release(samplers);
    heap = {};
}

void writeDescriptors(Device &device, DescriptorHeap &heap, std::initializer_list<DescriptorWrite> writes) {
    writeDescriptors(device, heap, std::span<const DescriptorWrite>(writes.begin(), writes.size()));
}

void writeDescriptors(Device &device, DescriptorHeap &heap, std::span<const DescriptorWrite> writes) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    const D3D12_CPU_DESCRIPTOR_HANDLE start =
        static_cast<ID3D12DescriptorHeap *>(heap.internal.resourceHeap)->GetCPUDescriptorHandleForHeapStart();

    for (const DescriptorWrite &write : writes) {
        EQ_VALIDATE(Validation::checkSlot(write.slot, heap.resourceCount, "resource"));
        const D3D12_CPU_DESCRIPTOR_HANDLE slot{start.ptr + static_cast<SIZE_T>(write.slot) * state.resourceDescriptorSize};
        if (writeTextureView(state, write.descriptor, slot)) {
            continue;
        }

        const auto [buffer, offset, size, stride, kind] = std::visit(
            [](const auto &view) {
                using View = std::decay_t<decltype(view)>;
                if constexpr (std::is_same_v<View, ConstantBufferView>) {
                    return std::tuple{view.buffer, view.offset, view.size, uint32{0}, 2};
                } else if constexpr (std::is_same_v<View, AccelerationStructureView>) {
                    return std::tuple{Buffer{}, uint64{0}, uint64{0}, uint32{0}, 0}; // written by writeTextureView
                } else if constexpr (std::is_same_v<View, ReadWriteView>) {
                    return std::tuple{view.buffer, view.offset, view.size, view.stride, 1};
                } else {
                    return std::tuple{view.buffer, view.offset, view.size, view.stride, 0};
                }
            },
            write.descriptor);
        EQ_DX_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
        const uint64 resolved = size == kWholeSize ? buffer.size - std::min(offset, buffer.size) : size;
        EQ_VALIDATE(Validation::checkBufferView(offset, resolved, buffer.size, stride, kind == 2));
        auto *resource = static_cast<ID3D12Resource *>(buffer.internal.resource);

        // Raw views count 4-byte elements; structured views count `stride`.
        const uint32 element = stride == 0 ? 4 : stride;
        const UINT64 first = offset / element;
        const UINT count = static_cast<UINT>(resolved / element);
        if (kind == 2) {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
                .BufferLocation = buffer.gpuAddress + offset,
                .SizeInBytes = static_cast<UINT>(Validation::constantViewSize(resolved))};
            state.device->CreateConstantBufferView(&cbv, slot);
        } else if (kind == 1) {
            const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
                .Format = stride == 0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN,
                .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                .Buffer = {.FirstElement = first,
                           .NumElements = count,
                           .StructureByteStride = stride,
                           .CounterOffsetInBytes = 0,
                           .Flags = stride == 0 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE}};
            state.device->CreateUnorderedAccessView(resource, nullptr, &uav, slot);
        } else {
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv{
                .Format = stride == 0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN,
                .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Buffer = {.FirstElement = first,
                           .NumElements = count,
                           .StructureByteStride = stride,
                           .Flags = stride == 0 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE}};
            state.device->CreateShaderResourceView(resource, &srv, slot);
        }
    }
}

// ── Import (P77) ─────────────────────────────────────────────────────────────

Memory importMemory(Device &device, ID3D12Heap *heap, MemoryKind kind, uint64 size, void *mapped) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    Memory memory;
    memory.kind = kind;
    memory.size = size;
    memory.mapped = mapped;
    memory.internal.heap = heap;
    memory.internal.imported = true;
    memory.internal.id = EQ_DX_TRACK(&state);
    return memory;
}

Buffer importBuffer(Device &device, ID3D12Resource *resource, const BufferDesc &desc, void *mapped) {
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    Buffer buffer;
    buffer.size = desc.size;
    buffer.usage = desc.usage;
    buffer.gpuAddress = resource->GetGPUVirtualAddress();
    buffer.mapped = mapped;
    buffer.internal.resource = resource;
    buffer.internal.imported = true;
    buffer.internal.id = EQ_DX_TRACK(&state);
    return buffer;
}

void writeNativeDescriptor(Device &device, DescriptorHeap &heap, uint32 slot, D3D12_CPU_DESCRIPTOR_HANDLE source) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    EQ_VALIDATE(Validation::checkSlot(slot, heap.resourceCount, "resource"));
    const D3D12_CPU_DESCRIPTOR_HANDLE start =
        static_cast<ID3D12DescriptorHeap *>(heap.internal.resourceHeap)->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE target{start.ptr + static_cast<SIZE_T>(slot) * state.resourceDescriptorSize};
    state.device->CopyDescriptorsSimple(1, target, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

} // namespace RI::D3D12
