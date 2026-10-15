#pragma once

// Portable recording and submission types (CONTEXT.md P20, P34, P36, P56).

#include <RI/Flags.hpp>
#include <types.hpp>

namespace RI {

// Pipeline stages for coarse barriers (P36): a barrier from `before` to
// `after` makes every write of the `before` stages visible to every access of
// the `after` stages. Graphics stages come with graphics pipelines.
enum class Stage : uint32 {
    eNone = 0,
    eCopy = rvl::eBit(0),
    eCompute = rvl::eBit(1),
    eIndirect = rvl::eBit(2), // reading indirect arguments (P48)
    eVertex = rvl::eBit(3),   // every shader before rasterization: vertex, hull, domain, geometry, task, mesh
    ePixel = rvl::eBit(4),    // pixel shading
    eRenderTarget = rvl::eBit(5),  // color attachment reads and writes
    eDepthStencil = rvl::eBit(6),  // depth/stencil tests and writes
    eAccelerationStructure = rvl::eBit(7), // acceleration structure builds (P49)
    eAll = rvl::eBit(31),
};
makeBitmask(Stage)

// Indirect execution (P48): what the GPU-written arguments describe. Each
// argument struct has the same layout on both backends.
enum class IndirectKind : uint8 { eDraw, eDrawIndexed, eDrawMesh, eDispatch };

struct DrawIndirectArgs {
    uint32 vertexCount;
    uint32 instanceCount;
    uint32 firstVertex;
    uint32 firstInstance;
};
struct DrawIndexedIndirectArgs {
    uint32 indexCount;
    uint32 instanceCount;
    uint32 firstIndex;
    int32 vertexOffset;
    uint32 firstInstance;
};
struct DispatchIndirectArgs { // also mesh draws: task (or mesh) groups
    uint32 x;
    uint32 y;
    uint32 z;
};

// Bytes from one indirect record to the next (P48): the arguments of `kind`,
// preceded at Level 2 by `rootDataSize` bytes of root data (rounded up to 4).
constexpr uint32 indirectStride(IndirectKind kind, uint32 rootDataSize = 0) {
    const uint32 args = kind == IndirectKind::eDraw          ? sizeof(DrawIndirectArgs)
                        : kind == IndirectKind::eDrawIndexed ? sizeof(DrawIndexedIndirectArgs)
                                                             : sizeof(DispatchIndirectArgs);
    return (rootDataSize + 3) / 4 * 4 + args;
}

// A point on a queue's GPU timeline (P20): reached when the GPU has finished
// the submission that returned it. Values only grow; 0 is always reached.
struct TimelinePoint {
    uint64 value = 0;
};

} // namespace RI
