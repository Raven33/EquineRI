// Memory, buffers and descriptor heaps (P31, P32, P33, P58, P59, P61, P87, P94).

#include "D3D12Check.hpp"
#include "DeviceState.hpp"

#include <RI/D3D12/Native.hpp>
#include <RI/Error.hpp>
#include <RI/Validation/Checks.hpp>

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

D3D12_RESOURCE_DESC1 bufferDesc(uint64 size, BufferUsage usage) {
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
        .Flags = (usage & BufferUsage::eReadWrite) != BufferUsage::eNone ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
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

} // namespace

// ── Memory ───────────────────────────────────────────────────────────────────

Memory allocateMemory(Device &device, MemoryKind kind, uint64 size) {
    DeviceState &state = *device.internal.state;
    if (kind == MemoryKind::eGPUUpload && !state.gpuUploadHeap) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("memory kind {} is not available on '{}' (see DeviceLimits::gpuUploadMemory)",
                                nameOf(kind), device.gpu.name));
    }
    const uint64 heapSize = alignUp(std::max<uint64>(size, 1), kPlacementAlignment);
    const D3D12_HEAP_DESC desc{
        .SizeInBytes = heapSize,
        .Properties = {.Type = heapTypeOf(kind)},
        .Alignment = kPlacementAlignment,
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
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_DX_UNTRACK(&state, memory.internal.id);
    auto *whole = static_cast<ID3D12Resource *>(memory.internal.mapBuffer);
    release(whole);
    auto *heap = static_cast<ID3D12Heap *>(memory.internal.heap);
    release(heap);
    memory = {};
}

// ── Buffers ──────────────────────────────────────────────────────────────────

MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    const D3D12_RESOURCE_DESC1 native = bufferDesc(desc.size, desc.usage);
    const D3D12_RESOURCE_ALLOCATION_INFO info = state.device->GetResourceAllocationInfo2(0, 1, &native, nullptr);
    return {.size = info.SizeInBytes, .alignment = info.Alignment};
}

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, memory.internal.id, "memory");
    const MemoryRequirements requirements = bufferRequirements(device, desc);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, alignUp(memory.size, kPlacementAlignment),
                                       requirements.alignment, "createBuffer: placing the buffer in memory"));

    ID3D12Resource *resource =
        placeBuffer(state, static_cast<ID3D12Heap *>(memory.internal.heap), offset, bufferDesc(desc.size, desc.usage));
    Buffer result;
    result.size = desc.size;
    result.usage = desc.usage;
    result.gpuAddress = resource->GetGPUVirtualAddress();
    result.mapped = memory.mapped != nullptr ? static_cast<std::byte *>(memory.mapped) + offset : nullptr;
    result.internal.resource = resource;
    result.internal.id = EQ_DX_TRACK(&state);
    return result;
}

void destroyBuffer(Device &device, Buffer &buffer) {
    DeviceState &state = *device.internal.state;
    EQ_DX_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
    EQ_DX_UNTRACK(&state, buffer.internal.id);
    auto *resource = static_cast<ID3D12Resource *>(buffer.internal.resource);
    release(resource);
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
    DeviceState &state = *device.internal.state;
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

        const auto [buffer, offset, size, stride, kind] = std::visit(
            [](const auto &view) {
                using View = std::decay_t<decltype(view)>;
                if constexpr (std::is_same_v<View, ConstantBufferView>) {
                    return std::tuple{view.buffer, view.offset, view.size, uint32{0}, 2};
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

} // namespace RI::D3D12
