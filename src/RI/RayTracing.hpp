#pragma once

// Inline ray tracing (CONTEXT.md P49): acceleration structures built by
// recorded commands into user-placed memory (P31), read by shaders through
// RayQuery with the top level in a descriptor heap slot (P42).
// DeviceLimits::rayTracing says whether the GPU has it.

#include <RI/Flags.hpp>
#include <RI/Textures.hpp>
#include <types.hpp>

#include <vector>

namespace RI {

enum class AccelerationStructureFlags : uint32 {
    eNone = 0,
    ePreferFastTrace = rvl::eBit(0),
    ePreferFastBuild = rvl::eBit(1),
    eAllowUpdate = rvl::eBit(2), // can be refit with `update` builds
};
makeBitmask(AccelerationStructureFlags)

// Triangles of a bottom level: float3 positions (`vertexStride` bytes apart),
// optionally indexed. Addresses are buffer GPU addresses (P61) of buffers with
// BufferUsage::eAccelerationStructureInput.
struct BlasTriangles {
    uint64 vertexAddress = 0;
    uint32 vertexStride = 12;
    uint32 vertexCount = 0;
    uint64 indexAddress = 0; // 0: not indexed (vertexCount / 3 triangles)
    IndexFormat indexFormat = IndexFormat::eUint32;
    uint32 indexCount = 0;
    bool opaque = true; // no any-hit processing
};

struct BlasDesc {
    std::vector<BlasTriangles> triangles;
    AccelerationStructureFlags flags = AccelerationStructureFlags::ePreferFastTrace;
};

// A top level: `instanceCount` AccelerationStructureInstance records at
// `instancesAddress` (a buffer with eAccelerationStructureInput).
struct TlasDesc {
    uint64 instancesAddress = 0;
    uint32 instanceCount = 0;
    AccelerationStructureFlags flags = AccelerationStructureFlags::ePreferFastTrace;
};

// The 64-byte instance record, the same on D3D12 and Vulkan (P49).
struct AccelerationStructureInstance {
    float transform[3][4];         // row-major 3x4: object to world
    uint32 instanceId : 24;        // InstanceID() in shaders
    uint32 mask : 8;               // ANDed with the ray's mask
    uint32 contributionOffset : 24; // hit-group offset (unused by inline ray tracing)
    uint32 flags : 8;              // instance flags (e.g. 0x4: force opaque)
    uint64 blasAddress;            // AccelerationStructure::gpuAddress of the bottom level
};
static_assert(sizeof(AccelerationStructureInstance) == 64);

// What a build needs (P49): the structure's size, and scratch for a build or
// an update.
struct AccelerationStructureSizes {
    uint64 size = 0;
    uint64 buildScratchSize = 0;
    uint64 updateScratchSize = 0;
};

} // namespace RI
