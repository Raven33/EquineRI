// Memory, buffers and descriptor heaps on Metal (P31, P32, P33, P58, P59, P61,
// P87, P94, P114 (3), P115).
//
// Owns: allocateMemory (one untracked placement heap per allocation, storage
// and cache mode from the kind), placed buffers, descriptor heaps (buffers of
// MSC's 24-byte IRDescriptorTableEntry), descriptor writes of buffer and
// texture views, and samplers (writeSamplers, the device's sampler cache).
// Acceleration-structure views come later, with ray tracing (notYet).
//
// Watch out: Metal 4 makes nothing resident on its own: every heap and heap
// buffer joins the device's residency set (buffers placed in a heap are
// covered by it). Metal maps buffers, not heaps: CPU-visible memory is mapped
// through one buffer placed over the whole heap, as on D3D12, and buffers
// placed later share it. Heap entries are written by RI itself (MSC's headers
// are not included), as IRDescriptorTableSet* would: buffers gpuVA and
// metadata = the view's length in bytes, which GetDimensions reads; textures
// the view's gpuResourceID; samplers the sampler's gpuResourceID in gpuVA.
// Sampler objects need supportArgumentBuffers, or Metal's debug layer asserts
// when RI reads their resource ID.
//
// D3D12 counterpart: backends/d3d12/Resources.cpp.

#include "DeviceState.hpp"

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <tuple>
#include <type_traits>
#include <variant>

namespace RI::Metal {

namespace {

const char *nameOf(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::eGPU: return "eGPU";
    case MemoryKind::eUpload: return "eUpload";
    case MemoryKind::eReadback: return "eReadback";
    case MemoryKind::eGPUUpload: return "eGPUUpload";
    }
    return "?";
}

// Upload kinds are written by the CPU and never read back by it: write-combined.
MTL::ResourceOptions optionsOf(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::eGPU: return MTL::ResourceStorageModePrivate;
    case MemoryKind::eUpload:
    case MemoryKind::eGPUUpload: return MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined;
    case MemoryKind::eReadback: return MTL::ResourceStorageModeShared;
    }
    return MTL::ResourceStorageModePrivate;
}

// A heap's descriptor buffer: CPU-written, GPU-read, synchronized by RI (P59).
MTL::Buffer *newHeapBuffer(DeviceState &state, uint32 count) {
    const uint64 size = uint64{std::max(count, 1u)} * sizeof(DescriptorEntry);
    MTL::Buffer *buffer =
        state.device->newBuffer(size, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined |
                                          MTL::ResourceHazardTrackingModeUntracked);
    if (buffer == nullptr) {
        reportError(ErrorCategory::eOutOfMemory,
                    std::format("createDescriptorHeap: Metal could not allocate a {}-byte heap buffer", size));
    }
    // Unwritten slots read as null descriptors rather than stale addresses.
    std::memset(buffer->contents(), 0, size);
    makeResident(state, buffer);
    return buffer;
}

#if EQUINE_VALIDATION
// The usage a buffer view needs: D3D12's UAVs and Vulkan's storage and uniform
// descriptors require it, and on Metal no layer sees RI's heap entries (P88).
// Local until the check moves to src/RI/Validation for every backend.
Validation::CheckResult checkViewUsage(const DescriptorWrite &write, BufferUsage usage) {
    const auto has = [&](BufferUsage wanted) { return (usage & wanted) != BufferUsage::eNone; };
    if (std::holds_alternative<ReadWriteView>(write.descriptor) && !has(BufferUsage::eReadWrite)) {
        return std::format("writeDescriptors: slot {}: a ReadWriteView needs a buffer with BufferUsage::eReadWrite",
                           write.slot);
    }
    if (std::holds_alternative<ReadView>(write.descriptor) && !has(BufferUsage::eRead | BufferUsage::eReadWrite)) {
        return std::format("writeDescriptors: slot {}: a ReadView needs a buffer with BufferUsage::eRead (or "
                           "eReadWrite)",
                           write.slot);
    }
    if (std::holds_alternative<ConstantBufferView>(write.descriptor) && !has(BufferUsage::eConstant)) {
        return std::format("writeDescriptors: slot {}: a ConstantBufferView needs a buffer with BufferUsage::eConstant",
                           write.slot);
    }
    return {};
}

// The usage a texture view needs: what D3D12's SRVs and UAVs and Vulkan's
// image descriptors require; Metal validates no heap entry (P88).
Validation::CheckResult checkTextureViewUsage(const DescriptorWrite &write, TextureUsage usage) {
    const bool storage = std::holds_alternative<ReadWriteView>(write.descriptor);
    const TextureUsage wanted = storage ? TextureUsage::eStorage : TextureUsage::eSampled;
    if ((usage & wanted) == TextureUsage::eNone) {
        return std::format("writeDescriptors: slot {}: a {} of a texture needs TextureUsage::{}", write.slot,
                           storage ? "ReadWriteView" : "ReadView", storage ? "eStorage" : "eSampled");
    }
    return {};
}
#endif

void destroyHeapBuffer(DeviceState &state, void *&pointer) {
    auto *buffer = static_cast<MTL::Buffer *>(pointer);
    if (buffer != nullptr) {
        makeNonResident(state, buffer);
        release(buffer);
    }
    pointer = nullptr;
}

} // namespace

// ── Memory ───────────────────────────────────────────────────────────────────

// Metal has one storage mode per kind, and placement heaps hold buffers and
// textures alike: `forResource` changes nothing.
Memory allocateMemory(Device &device, MemoryKind kind, uint64 size, const MemoryRequirements *,
                      std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    if (kind == MemoryKind::eGPUUpload && !device.limits.gpuUploadMemory) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("memory kind {} is not available on '{}' (see DeviceLimits::gpuUploadMemory)",
                                nameOf(kind), device.gpu.name));
    }
    const MTL::ResourceOptions options = optionsOf(kind);
    const uint64 heapSize = alignUp(std::max<uint64>(size, 1), kHeapGranularity);
    MTL::HeapDescriptor *desc = MTL::HeapDescriptor::alloc()->init();
    desc->setType(MTL::HeapTypePlacement);
    desc->setSize(heapSize);
    desc->setResourceOptions(options | MTL::ResourceHazardTrackingModeUntracked); // RI synchronizes (P36)
    MTL::Heap *heap = state.device->newHeap(desc);
    desc->release();
    if (heap == nullptr) {
        reportError(ErrorCategory::eOutOfMemory,
                    std::format("allocateMemory: Metal could not allocate {} bytes of {} memory", heapSize,
                                nameOf(kind)));
    }

    Memory result;
    result.kind = kind;
    result.size = size;
    // CPU-visible kinds stay mapped for their lifetime (P33). Metal maps
    // buffers, not heaps, so RI maps one buffer over the whole heap.
    if (kind != MemoryKind::eGPU) {
        MTL::Buffer *whole = heap->newBuffer(heap->size(), heap->resourceOptions(), 0);
        if (whole == nullptr) {
            heap->release();
            reportError(ErrorCategory::eOutOfMemory,
                        std::format("allocateMemory: Metal could not map {} memory of {} bytes", nameOf(kind),
                                    heapSize));
        }
        result.mapped = whole->contents();
        result.internal.mapBuffer = whole;
    }
    makeResident(state, heap);
    result.internal.heap = heap;
    result.internal.id = EQ_MTL_TRACK(&state);
    if (!name.empty()) {
        applyName(state, result, name);
    }
    return result;
}

void freeMemory(Device &device, Memory &memory) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_MTL_UNTRACK(&state, memory.internal.id);
#if EQUINE_VALIDATION
    Validation::forgetMemory(state.placements, memory.internal.id);
#endif
    if (!memory.internal.imported) { // P77: an imported heap stays the user's
        auto *whole = static_cast<MTL::Buffer *>(memory.internal.mapBuffer);
        release(whole);
        auto *heap = static_cast<MTL::Heap *>(memory.internal.heap);
        if (heap != nullptr) {
            makeNonResident(state, heap);
        }
        release(heap);
    }
    memory = {};
}

// ── Buffers ──────────────────────────────────────────────────────────────────

// One answer for every kind: the largest size and alignment Metal asks for
// across the storage modes, so a buffer fits whichever memory the user picks.
MemoryRequirements bufferRequirements(Device &device, const BufferDesc &desc) {
    // Capability gates where Vulkan puts them, with its messages.
    if ((desc.usage & BufferUsage::eAccelerationStructureInput) != BufferUsage::eNone && !device.limits.rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    "BufferUsage::eAccelerationStructureInput needs ray tracing (DeviceLimits::rayTracing)");
    }
    if ((desc.usage & BufferUsage::eIndirectScratch) != BufferUsage::eNone && !device.limits.indirectRootData) {
        reportError(ErrorCategory::eUnsupported,
                    "BufferUsage::eIndirectScratch needs indirect Level 2 (DeviceLimits::indirectRootData)");
    }
    DeviceState &state = *device.internal.state;
    uint64 size = 0;
    uint64 alignment = 1;
    uint32 kinds = 0;
    for (MemoryKind kind : {MemoryKind::eGPU, MemoryKind::eUpload, MemoryKind::eReadback, MemoryKind::eGPUUpload}) {
        if (kind == MemoryKind::eGPUUpload && !device.limits.gpuUploadMemory) {
            continue;
        }
        const MTL::SizeAndAlign native = state.device->heapBufferSizeAndAlign(
            std::max<uint64>(desc.size, 1), optionsOf(kind) | MTL::ResourceHazardTrackingModeUntracked);
        size = std::max<uint64>(size, native.size);
        alignment = std::max<uint64>(alignment, native.align);
        kinds |= 1u << static_cast<uint32>(kind);
    }
    return {.size = alignUp(size, alignment), .alignment = alignment, .kinds = kinds};
}

Buffer createBuffer(Device &device, const Memory &memory, uint64 offset, const BufferDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, memory.internal.id, "memory");
    auto *heap = static_cast<MTL::Heap *>(memory.internal.heap);
    [[maybe_unused]] const MemoryRequirements requirements = bufferRequirements(device, desc);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, alignUp(memory.size, kHeapGranularity),
                                       requirements.alignment, "createBuffer: placing the buffer in memory"));

    MTL::Buffer *buffer = heap->newBuffer(std::max<uint64>(desc.size, 1), heap->resourceOptions(), offset);
    if (buffer == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createBuffer: Metal could not place a {}-byte buffer at offset {} in memory of {} "
                                "bytes",
                                desc.size, offset, memory.size));
    }
    Buffer result;
    result.size = desc.size;
    result.usage = desc.usage;
    result.gpuAddress = buffer->gpuAddress();
    result.mapped = memory.mapped != nullptr ? static_cast<std::byte *>(memory.mapped) + offset : nullptr;
    result.internal.buffer = buffer;
    result.internal.id = EQ_MTL_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, result.internal.id,
                              false);
#endif
    if (!desc.name.empty()) {
        applyName(state, result, desc.name);
    }
    return result;
}

void destroyBuffer(Device &device, Buffer &buffer) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
    EQ_MTL_UNTRACK(&state, buffer.internal.id);
    if (!buffer.internal.imported) { // P77: an imported buffer stays the user's
        auto *native = static_cast<MTL::Buffer *>(buffer.internal.buffer);
        release(native);
    }
    buffer = {};
}

// ── Descriptor heaps (P94, P114 (3)) ─────────────────────────────────────────

DescriptorHeap createDescriptorHeap(Device &device, const DescriptorHeapDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    if (desc.resourceCount > device.limits.maxResourceDescriptors ||
        desc.samplerCount > device.limits.maxSamplerDescriptors) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createDescriptorHeap: {} resource / {} sampler slots exceed the device limits "
                                "({} / {}, Device::limits)",
                                desc.resourceCount, desc.samplerCount, device.limits.maxResourceDescriptors,
                                device.limits.maxSamplerDescriptors));
    }
    DescriptorHeap heap;
    heap.resourceCount = desc.resourceCount;
    heap.samplerCount = desc.samplerCount;
    heap.internal.resourceBuffer = newHeapBuffer(state, desc.resourceCount);
    heap.internal.samplerBuffer = newHeapBuffer(state, desc.samplerCount);
    heap.internal.id = EQ_MTL_TRACK(&state);
    if (!desc.name.empty()) {
        applyName(state, heap, desc.name);
    }
    return heap;
}

void destroyDescriptorHeap(Device &device, DescriptorHeap &heap) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    EQ_MTL_UNTRACK(&state, heap.internal.id);
    destroyHeapBuffer(state, heap.internal.resourceBuffer);
    destroyHeapBuffer(state, heap.internal.samplerBuffer);
    heap = {};
}

// ── Samplers (P87, P99, P114 (3)) ────────────────────────────────────────────

namespace {

std::array<uint32, 13> samplerKey(const SamplerDesc &d) {
    return {static_cast<uint32>(d.minFilter),  static_cast<uint32>(d.magFilter),
            static_cast<uint32>(d.mipFilter),  static_cast<uint32>(d.addressU),
            static_cast<uint32>(d.addressV),   static_cast<uint32>(d.addressW),
            std::bit_cast<uint32>(d.mipLodBias), std::bit_cast<uint32>(d.minLod),
            std::bit_cast<uint32>(d.maxLod),   d.maxAnisotropy,
            d.compare.has_value() ? 1u + static_cast<uint32>(*d.compare) : 0u,
            static_cast<uint32>(d.border),     static_cast<uint32>(d.reduction)};
}

MTL::SamplerAddressMode addressOf(AddressMode mode) {
    switch (mode) {
    case AddressMode::eRepeat: return MTL::SamplerAddressModeRepeat;
    case AddressMode::eMirror: return MTL::SamplerAddressModeMirrorRepeat;
    case AddressMode::eClamp: return MTL::SamplerAddressModeClampToEdge;
    case AddressMode::eBorder: return MTL::SamplerAddressModeClampToBorderColor;
    }
    return MTL::SamplerAddressModeRepeat;
}

MTL::SamplerBorderColor borderOf(BorderColor color) {
    switch (color) {
    case BorderColor::eTransparentBlack: return MTL::SamplerBorderColorTransparentBlack;
    case BorderColor::eOpaqueBlack: return MTL::SamplerBorderColorOpaqueBlack;
    case BorderColor::eOpaqueWhite: return MTL::SamplerBorderColorOpaqueWhite;
    }
    return MTL::SamplerBorderColorTransparentBlack;
}

// The device's sampler for `desc`, created on first use.
MTL::SamplerState *samplerFor(DeviceState &state, const SamplerDesc &desc) {
    const std::array<uint32, 13> key = samplerKey(desc);
    if (const auto found = state.samplers.find(key); found != state.samplers.end()) {
        return found->second;
    }
    // Anisotropy filters linearly, mips included, as D3D12's anisotropic filter does.
    const bool anisotropic = desc.maxAnisotropy > 1;
    const auto filter = [&](Filter f) {
        return f == Filter::eLinear || anisotropic ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest;
    };
    MTL::SamplerDescriptor *native = MTL::SamplerDescriptor::alloc()->init();
    native->setMinFilter(filter(desc.minFilter));
    native->setMagFilter(filter(desc.magFilter));
    native->setMipFilter(desc.mipFilter == Filter::eLinear || anisotropic ? MTL::SamplerMipFilterLinear
                                                                          : MTL::SamplerMipFilterNearest);
    native->setSAddressMode(addressOf(desc.addressU));
    native->setTAddressMode(addressOf(desc.addressV));
    native->setRAddressMode(addressOf(desc.addressW));
    native->setBorderColor(borderOf(desc.border));
    native->setLodMinClamp(desc.minLod);
    native->setLodMaxClamp(desc.maxLod);
    // The bias lives in the sampler: MSC reads the heap entry's bias only with
    // --samplerLODBias, which RI does not pass (P114 (3)).
    native->setLodBias(desc.mipLodBias);
    native->setMaxAnisotropy(std::clamp<uint32>(desc.maxAnisotropy, 1, 16));
    // RI's CompareOp has Metal's order (Never ... Always).
    if (desc.compare.has_value()) {
        native->setCompareFunction(static_cast<MTL::CompareFunction>(*desc.compare));
    }
    native->setReductionMode(desc.reduction == Reduction::eMin   ? MTL::SamplerReductionModeMinimum
                             : desc.reduction == Reduction::eMax ? MTL::SamplerReductionModeMaximum
                                                                 : MTL::SamplerReductionModeWeightedAverage);
    native->setNormalizedCoordinates(true);
    native->setSupportArgumentBuffers(true); // MSC reads samplers through the heap
    MTL::SamplerState *sampler = state.device->newSamplerState(native);
    native->release();
    if (sampler == nullptr) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("writeSamplers: Metal refused a new sampler after {} distinct ones",
                                state.samplers.size()));
    }
    state.samplers.emplace(key, sampler);
    return sampler;
}

} // namespace

void writeSamplers(Device &device, DescriptorHeap &heap, std::initializer_list<SamplerWrite> writes) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    writeSamplers(device, heap, std::span<const SamplerWrite>(writes.begin(), writes.size()));
}

void writeSamplers(Device &device, DescriptorHeap &heap, std::span<const SamplerWrite> writes) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    auto *entries = static_cast<DescriptorEntry *>(static_cast<MTL::Buffer *>(heap.internal.samplerBuffer)->contents());
    for (const SamplerWrite &write : writes) {
        EQ_VALIDATE(Validation::checkSlot(write.slot, heap.samplerCount, "sampler"));
        EQ_VALIDATE(Validation::checkSampler(write.sampler, device.limits.samplerMinMax));
        const MTL::SamplerState *sampler = samplerFor(state, write.sampler);
        entries[write.slot] = DescriptorEntry{
            .gpuVA = sampler->gpuResourceID()._impl,
            .textureViewID = 0,
            .metadata = std::bit_cast<uint32>(write.sampler.mipLodBias),
        };
    }
}

// ── Descriptor writes (P87, P94) ─────────────────────────────────────────────

void writeDescriptors(Device &device, DescriptorHeap &heap, std::initializer_list<DescriptorWrite> writes) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    writeDescriptors(device, heap, std::span<const DescriptorWrite>(writes.begin(), writes.size()));
}

void writeDescriptors(Device &device, DescriptorHeap &heap, std::span<const DescriptorWrite> writes) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    [[maybe_unused]] DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, heap.internal.id, "descriptor heap");
    auto *entries = static_cast<DescriptorEntry *>(static_cast<MTL::Buffer *>(heap.internal.resourceBuffer)->contents());

    for (const DescriptorWrite &write : writes) {
        EQ_VALIDATE(Validation::checkSlot(write.slot, heap.resourceCount, "resource"));
        if (const auto *tlas = std::get_if<AccelerationStructureView>(&write.descriptor)) {
            entries[write.slot] = accelerationStructureEntry(state, tlas->tlas);
            continue;
        }
        // A read or read-write view names a texture when its texture view is set.
        const auto *read = std::get_if<ReadView>(&write.descriptor);
        const auto *readWrite = std::get_if<ReadWriteView>(&write.descriptor);
        const TextureView *texture = read != nullptr && read->texture.internal.texture != nullptr ? &read->texture
                                     : readWrite != nullptr && readWrite->texture.internal.texture != nullptr
                                         ? &readWrite->texture
                                         : nullptr;
        if (texture != nullptr) {
            EQ_MTL_CHECK_ALIVE(&state, texture->internal.textureId, "texture");
            EQ_VALIDATE(checkTextureViewUsage(write, texture->internal.textureUsage));
            // In every build: Metal has no view reading depth and stencil at once.
            if (texture->internal.planes == 3) {
                reportError(ErrorCategory::eProgrammerError,
                            "writeDescriptors: a view with both depth and stencil cannot be read by shaders; create "
                            "a view with Aspect::eDepth or Aspect::eStencil (P73)");
            }
            // Storage writes see whole cubes as a 2D array of faces (P102).
            const void *view = readWrite != nullptr && texture->internal.arrayTexture != nullptr
                                   ? texture->internal.arrayTexture
                                   : texture->internal.texture;
            // No min LOD clamp: the view's mip range already bounds sampling.
            entries[write.slot] = DescriptorEntry{
                .gpuVA = 0,
                .textureViewID = static_cast<const MTL::Texture *>(view)->gpuResourceID()._impl,
                .metadata = 0,
            };
            continue;
        }

        const auto [buffer, offset, size, stride, constant] = std::visit(
            [](const auto &view) {
                using View = std::decay_t<decltype(view)>;
                if constexpr (std::is_same_v<View, ConstantBufferView>) {
                    return std::tuple{view.buffer, view.offset, view.size, uint32{0}, true};
                } else if constexpr (std::is_same_v<View, AccelerationStructureView>) {
                    return std::tuple{Buffer{}, uint64{0}, uint64{0}, uint32{0}, false}; // reported above
                } else {
                    return std::tuple{view.buffer, view.offset, view.size, view.stride, false};
                }
            },
            write.descriptor);
        EQ_MTL_CHECK_ALIVE(&state, buffer.internal.id, "buffer");
        EQ_VALIDATE(checkViewUsage(write, buffer.usage));
        const uint64 resolved = size == kWholeSize ? buffer.size - std::min(offset, buffer.size) : size;
        EQ_VALIDATE(Validation::checkBufferView(offset, resolved, buffer.size, stride, constant));
        EQ_VALIDATE(Validation::checkBufferViewSize(resolved, stride, constant, device.limits.maxBufferViewSize,
                                                    device.limits.maxConstantViewSize));
        // The entry holds the length in 32 bits; maxBufferViewSize keeps views
        // below that, and release builds must not truncate one silently.
        if (resolved > 0xFFFF'FFFFu) {
            reportError(ErrorCategory::eUnsupported,
                        std::format("writeDescriptors: a buffer view of {} bytes: Metal views hold at most {} bytes "
                                    "(DeviceLimits::maxBufferViewSize)",
                                    resolved, device.limits.maxBufferViewSize));
        }

        // Raw, structured and constant views alike: MSC's shaders take the
        // address and the length in bytes; the stride lives in the shader.
        // Constant views keep their exact length (D3D12 rounds to 256 bytes,
        // which could reach past the buffer here).
        entries[write.slot] = DescriptorEntry{
            .gpuVA = buffer.gpuAddress + offset,
            .textureViewID = 0,
            .metadata = resolved,
        };
    }
}

} // namespace RI::Metal
