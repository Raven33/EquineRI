// Inline ray tracing on Metal (P49, P116 (4), proposal Q5).
//
// Owns: acceleration structures placed in Memory heaps, their size queries
// and requirements, cmdBuildBlas / cmdBuildTlas (with update, as refits) on
// the compute encoder, and the descriptor entry of a top level
// (writeAccelerationStructureEntry, for writeDescriptors).
//
// How: a bottom level is an MTL4PrimitiveAccelerationStructureDescriptor of
// triangle geometries; a top level an MTL4InstanceAccelerationStructureDescriptor
// of Metal's indirect instances (72 bytes, naming bottom levels by resource
// ID). AccelerationStructure::gpuAddress is that resource ID: the value RI's
// 64-byte instances name. cmdBuildTlas repacks RI's instances into Metal's
// with RI's built-in kernel (Indirect.cpp) into the first
// kInstanceArea(count) bytes of the scratch, which tlasSizes adds.
//
// MSC's shaders reach a top level through a 64-byte
// IRRaytracingAccelerationStructureGPUHeader (the structure's resource ID and
// the address of the hit-group contributions, which ray queries do not read),
// whose address is the heap entry's gpuVA. It lives in the structure's own
// range (P116 (4)): accelerationStructureRequirements adds kHeaderSize bytes
// after the structure, a buffer placed there holds the header and zeroed
// contributions, and cmdBuildTlas writes them on the GPU (the memory is
// GPU-only), so a top level is readable once built.
//
// Watch out: the repack is a dispatch and the header a copy, so on Metal
// Stage::eAccelerationStructure also covers Dispatch and Blit (stagesOf).
// Structures are added to the residency set, as texture views: shader
// validation needs every resource a shader reaches listed there.
//
// D3D12 counterpart: backends/d3d12/RayTracing.cpp.

#include "DeviceState.hpp"

#include <Metal/MTL4AccelerationStructure.hpp>

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

constexpr uint64 kMetalInstanceSize = 72; // MTLIndirectAccelerationStructureInstanceDescriptor
constexpr uint64 kScratchAlignment = 256;
constexpr uint64 kHeaderSize = 128; // the 64-byte header, then zeroed contributions
constexpr uint32 kRepackGroupSize = 64;

// Metal's debug layer refuses an empty instance range, so a top level of 0
// instances keeps room for one record, which the build reads none of.
uint64 instanceRecords(uint32 count) { return std::max<uint64>(count, 1); }
uint64 instanceArea(uint32 count) { return alignUp(kMetalInstanceSize * instanceRecords(count), kScratchAlignment); }

struct RepackParams {
    uint64 src, dst;
    uint32 count, pad;
};

// MSC's IRRaytracingAccelerationStructureGPUHeader.
struct Header {
    uint64 accelerationStructureID;
    uint64 addressOfInstanceContributions;
    uint64 pad0[4];
    uint32 pad1[3];
    uint32 pad2;
};
static_assert(sizeof(Header) == 64);

void requireRayTracing(const DeviceState &state, const char *call) {
    if (!state.rayTracing) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: this GPU has no inline ray tracing (DeviceLimits::rayTracing)", call));
    }
}

MTL::AccelerationStructureUsage usageOf(AccelerationStructureFlags flags) {
    NS::UInteger usage = MTL::AccelerationStructureUsageNone;
    if ((flags & AccelerationStructureFlags::ePreferFastBuild) != AccelerationStructureFlags::eNone) {
        usage |= MTL::AccelerationStructureUsagePreferFastBuild;
    }
    if ((flags & AccelerationStructureFlags::eAllowUpdate) != AccelerationStructureFlags::eNone) {
        usage |= MTL::AccelerationStructureUsageRefit;
    }
    return static_cast<MTL::AccelerationStructureUsage>(usage);
}

// The native description of a build; released by the caller's autorelease pool.
MTL4::PrimitiveAccelerationStructureDescriptor *blasDescriptor(const BlasDesc &desc) {
    std::vector<NS::Object *> geometries;
    for (const BlasTriangles &t : desc.triangles) {
        auto *g = MTL4::AccelerationStructureTriangleGeometryDescriptor::alloc()->init()->autorelease();
        g->setVertexBuffer(MTL4::BufferRange(t.vertexAddress, uint64{t.vertexStride} * t.vertexCount));
        g->setVertexStride(t.vertexStride);
        g->setVertexFormat(MTL::AttributeFormatFloat3);
        g->setOpaque(t.opaque);
        if (t.indexAddress != 0) {
            const uint64 indexSize = t.indexFormat == IndexFormat::eUint16 ? 2 : 4;
            g->setIndexBuffer(MTL4::BufferRange(t.indexAddress, indexSize * t.indexCount));
            g->setIndexType(t.indexFormat == IndexFormat::eUint16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32);
            g->setTriangleCount(t.indexCount / 3);
        } else {
            g->setTriangleCount(t.vertexCount / 3);
        }
        geometries.push_back(g);
    }
    auto *out = MTL4::PrimitiveAccelerationStructureDescriptor::alloc()->init()->autorelease();
    out->setGeometryDescriptors(NS::Array::array(geometries.data(), geometries.size()));
    out->setUsage(usageOf(desc.flags));
    return out;
}

// `instances`: the repacked records (0 for size queries).
MTL4::InstanceAccelerationStructureDescriptor *tlasDescriptor(const TlasDesc &desc, uint64 instances) {
    auto *out = MTL4::InstanceAccelerationStructureDescriptor::alloc()->init()->autorelease();
    out->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
    out->setInstanceDescriptorStride(kMetalInstanceSize);
    out->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
    out->setInstanceCount(desc.instanceCount);
    if (instances != 0) {
        out->setInstanceDescriptorBuffer(
            MTL4::BufferRange(instances, kMetalInstanceSize * instanceRecords(desc.instanceCount)));
    }
    out->setUsage(usageOf(desc.flags));
    return out;
}

AccelerationStructureSizes sizesOf(DeviceState &state, MTL::AccelerationStructureDescriptor *desc, uint64 extra) {
    const MTL::AccelerationStructureSizes sizes = state.device->accelerationStructureSizes(desc);
    return {.size = sizes.accelerationStructureSize,
            .buildScratchSize = extra + sizes.buildScratchBufferSize,
            .updateScratchSize = extra + sizes.refitScratchBufferSize};
}

MTL::AccelerationStructure *structureOf(const AccelerationStructure &s) {
    return static_cast<MTL::AccelerationStructure *>(s.internal.structure);
}

// Where the header buffer goes, from the structure's offset and size.
struct HeaderPlacement {
    uint64 offset; // after the structure, from its start
    uint64 size;   // the header buffer's size in the heap
};
HeaderPlacement headerPlacement(DeviceState &state, uint64 nativeSize) {
    const MTL::SizeAndAlign buffer =
        state.device->heapBufferSizeAndAlign(kHeaderSize, MTL::ResourceStorageModePrivate |
                                                              MTL::ResourceHazardTrackingModeUntracked);
    return {.offset = alignUp(nativeSize, buffer.align), .size = alignUp(buffer.size, buffer.align)};
}

void checkBuild(CommandListState &list, DeviceState &state, [[maybe_unused]] const AccelerationStructure &dst,
                [[maybe_unused]] const Buffer &scratch, [[maybe_unused]] uint64 scratchOffset,
                [[maybe_unused]] uint64 scratchSize, const char *call) {
    requireRayTracing(state, call);
    EQ_MTL_CHECK_ALIVE(&state, dst.internal.id, "acceleration structure");
    EQ_MTL_CHECK_ALIVE(&state, scratch.internal.id, "buffer");
    if (list.rendering) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: not allowed inside a rendering section", call));
    }
    EQ_VALIDATE(Validation::checkRange(scratchOffset, scratchSize, scratch.size, 1,
                                       std::format("{}: the scratch range", call)));
    EQ_VALIDATE(Validation::checkRange(scratch.gpuAddress + scratchOffset, 0, UINT64_MAX, kScratchAlignment,
                                       std::format("{}: the scratch address (buffer GPU address + offset)", call)));
}

void encodeBuild(MTL4::ComputeCommandEncoder *encoder, const AccelerationStructure &dst,
                 MTL4::AccelerationStructureDescriptor *desc, uint64 scratch, uint64 scratchSize, bool update) {
    MTL::AccelerationStructure *structure = structureOf(dst);
    const MTL4::BufferRange range(scratch, scratchSize);
    if (update) {
        encoder->refitAccelerationStructure(structure, desc, structure, range);
    } else {
        encoder->buildAccelerationStructure(structure, desc, range);
    }
}

} // namespace

AccelerationStructureSizes blasSizes(Device &device, const BlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "blasSizes");
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    const AccelerationStructureSizes sizes = sizesOf(state, blasDescriptor(desc), 0);
    pool->release();
    return sizes;
}

// The scratch starts with the repacked instances.
AccelerationStructureSizes tlasSizes(Device &device, const TlasDesc &desc) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "tlasSizes");
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    const AccelerationStructureSizes sizes = sizesOf(state, tlasDescriptor(desc, 0), instanceArea(desc.instanceCount));
    pool->release();
    return sizes;
}

// Metal's placement of the structure, then the header buffer (P116 (4)); eGPU memory.
MemoryRequirements accelerationStructureRequirements(Device &device, uint64 size) {
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "accelerationStructureRequirements");
    const MTL::SizeAndAlign native = state.device->heapAccelerationStructureSizeAndAlign(std::max<uint64>(size, 1));
    const HeaderPlacement header = headerPlacement(state, native.size);
    const uint64 alignment = std::max<uint64>(native.align, kHeapGranularity);
    return {.size = alignUp(header.offset + header.size, alignment),
            .alignment = alignment,
            .kinds = 1u << static_cast<uint32>(MemoryKind::eGPU)};
}

AccelerationStructure createAccelerationStructure(Device &device, const Memory &memory, uint64 offset, uint64 size,
                                                  bool topLevel, std::string_view name) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    requireRayTracing(state, "createAccelerationStructure");
    EQ_MTL_CHECK_ALIVE(&state, memory.internal.id, "memory");
    [[maybe_unused]] const MemoryRequirements requirements = accelerationStructureRequirements(device, size);
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, alignUp(memory.size, kHeapGranularity),
                                       requirements.alignment, "createAccelerationStructure: placing it in memory"));
    if (memory.kind != MemoryKind::eGPU) {
        reportError(ErrorCategory::eProgrammerError,
                    "createAccelerationStructure: acceleration structures need MemoryKind::eGPU memory");
    }
    auto *heap = static_cast<MTL::Heap *>(memory.internal.heap);
    const MTL::SizeAndAlign native = state.device->heapAccelerationStructureSizeAndAlign(std::max<uint64>(size, 1));
    MTL::AccelerationStructure *structure = heap->newAccelerationStructure(native.size, offset);
    if (structure == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createAccelerationStructure: Metal could not place a {}-byte structure at offset {}",
                                size, offset));
    }
    MTL::Buffer *header = nullptr;
    if (topLevel) {
        const HeaderPlacement placement = headerPlacement(state, native.size);
        header = heap->newBuffer(kHeaderSize, heap->resourceOptions(), offset + placement.offset);
        if (header == nullptr) {
            reportError(ErrorCategory::eProgrammerError,
                        "createAccelerationStructure: Metal could not place the top level's header");
        }
    }
    makeResident(state, structure);
    AccelerationStructure out;
    out.size = size;
    out.gpuAddress = structure->gpuResourceID()._impl;
    out.topLevel = topLevel;
    out.internal.structure = structure;
    out.internal.header = header;
    out.internal.id = EQ_MTL_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, out.internal.id,
                              false);
#endif
    if (!name.empty()) {
        structure->setLabel(nsString(name));
    }
    return out;
}

void destroyAccelerationStructure(Device &device, AccelerationStructure &structure) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, structure.internal.id, "acceleration structure");
    EQ_MTL_UNTRACK(&state, structure.internal.id);
    MTL::AccelerationStructure *native = structureOf(structure);
    makeNonResident(state, native);
    release(native);
    auto *header = static_cast<MTL::Buffer *>(structure.internal.header);
    release(header);
    structure = {};
}

void cmdBuildBlas(CommandList &cmd, const AccelerationStructure &dst, const BlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    CommandListState &list = *cmd.internal.state;
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL4::PrimitiveAccelerationStructureDescriptor *native = blasDescriptor(desc);
    const AccelerationStructureSizes sizes = sizesOf(state, native, 0);
    const uint64 scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    checkBuild(list, state, dst, scratch, scratchOffset, scratchSize, "cmdBuildBlas");
    encodeBuild(computeEncoder(list), dst, native, scratch.gpuAddress + scratchOffset, std::max<uint64>(scratchSize, 1),
                update);
    pool->release();
}

// Repack (a dispatch), then the header (a copy) and the build, ordered by an
// encoder barrier.
void cmdBuildTlas(CommandList &cmd, const AccelerationStructure &dst, const TlasDesc &desc, const Buffer &scratch,
                  uint64 scratchOffset, bool update) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    CommandListState &list = *cmd.internal.state;
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    const uint64 instances = scratch.gpuAddress + scratchOffset;
    const uint64 area = instanceArea(desc.instanceCount);
    MTL4::InstanceAccelerationStructureDescriptor *native = tlasDescriptor(desc, instances);
    const AccelerationStructureSizes sizes = sizesOf(state, native, area);
    const uint64 scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    checkBuild(list, state, dst, scratch, scratchOffset, scratchSize, "cmdBuildTlas");
    if (!dst.topLevel) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdBuildTlas: the structure was created as a bottom level (createAccelerationStructure)");
    }
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);

    if (desc.instanceCount > 0) {
        const CommandMemoryAllocation params = allocateCommandMemory(state, list, sizeof(RepackParams), 16);
        const RepackParams p{.src = desc.instancesAddress, .dst = instances, .count = desc.instanceCount, .pad = 0};
        std::memcpy(params.cpu, &p, sizeof(p));
        list.argumentTable->setAddress(params.gpuAddress, kResourceHeapIndex);
        encoder->setComputePipelineState(builtinPipelines(state).repackKernel);
        encoder->dispatchThreadgroups(MTL::Size((desc.instanceCount + kRepackGroupSize - 1) / kRepackGroupSize, 1, 1),
                                      MTL::Size(kRepackGroupSize, 1, 1));
        if (list.pipeline != nullptr) {
            encoder->setComputePipelineState(list.pipeline);
        }
        if (list.resourceHeapAddress != 0) {
            list.argumentTable->setAddress(list.resourceHeapAddress, kResourceHeapIndex);
        }
        encoder->barrierAfterEncoderStages(MTL::StageDispatch, MTL::StageAccelerationStructure,
                                           MTL4::VisibilityOptionDevice);
    }

    // The header, from zeroed command memory (MSC's IRRaytracingSetAccelerationStructure).
    auto *header = static_cast<MTL::Buffer *>(dst.internal.header);
    const CommandMemoryAllocation source = allocateCommandMemory(state, list, kHeaderSize, 16);
    std::memset(source.cpu, 0, kHeaderSize);
    const Header h{.accelerationStructureID = dst.gpuAddress,
                   .addressOfInstanceContributions = header->gpuAddress() + sizeof(Header)};
    std::memcpy(source.cpu, &h, sizeof(h));
    encoder->copyFromBuffer(source.buffer, source.offset, header, 0, kHeaderSize);

    encodeBuild(encoder, dst, native, instances + area, std::max<uint64>(scratchSize - area, 1), update);
    pool->release();
}

// A top level's heap entry: its header's address (MSC's
// IRDescriptorTableSetAccelerationStructure).
DescriptorEntry accelerationStructureEntry(DeviceState &state, const AccelerationStructure &tlas) {
    requireRayTracing(state, "writeDescriptors");
    EQ_MTL_CHECK_ALIVE(&state, tlas.internal.id, "acceleration structure");
    if (!tlas.topLevel || tlas.internal.header == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    "writeDescriptors: an AccelerationStructureView names a top level (createAccelerationStructure "
                    "with topLevel)");
    }
    return {.gpuVA = static_cast<MTL::Buffer *>(tlas.internal.header)->gpuAddress(), .textureViewID = 0, .metadata = 0};
}

} // namespace RI::Metal
