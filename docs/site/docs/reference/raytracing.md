---
symbols: [RI::AccelerationStructure, RI::blasSizes, RI::tlasSizes, RI::accelerationStructureRequirements, RI::createAccelerationStructure, RI::destroyAccelerationStructure, RI::cmdBuildBlas, RI::cmdBuildTlas, RI::BlasDesc, RI::BlasTriangles, RI::TlasDesc, RI::AccelerationStructureInstance, RI::AccelerationStructureSizes, RI::AccelerationStructureFlags, RI::AccelerationStructureView]
---

# Inline ray tracing: acceleration structures

## What it does

**Inline ray tracing** lets any shader cast rays with HLSL `RayQuery` against an
**acceleration structure** ([P49](../design-notes/decisions.md#p49)). Available when
`device.limits.rayTracing` is set (Vulkan `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`,
D3D12 raytracing tier 1.1). On Vulkan the driver must also accept acceleration structures among
the heap's mutable descriptor types, since shaders read them from the same binding as every
other heap descriptor; the AMD Windows driver does not, so ray tracing is off there on Vulkan
(D3D12 is unaffected).

The explicit form, as everywhere in RI:

1. **Describe** the geometry: a `BlasDesc` lists triangle sets (`BlasTriangles`: float3
   positions at a buffer GPU address, optionally indexed); a `TlasDesc` names a buffer of
   `AccelerationStructureInstance` records (the 64-byte format both APIs share: a 3x4
   transform, an instance id, a mask, flags, the bottom level's `gpuAddress`).
2. **Ask the sizes**: `RI::blasSizes` / `RI::tlasSizes` give the structure size and the
   scratch sizes for a build and for an update.
3. **Place** the structure in your memory: `RI::accelerationStructureRequirements(device, size)`
   then `RI::createAccelerationStructure(device, memory, offset, size, topLevel)`
   ([P31](../design-notes/decisions.md#p31)).
4. **Build** in a command list: `RI::cmdBuildBlas` / `RI::cmdBuildTlas` with a scratch buffer
   (`BufferUsage::eReadWrite`). `update = true` refits a structure created with
   `AccelerationStructureFlags::eAllowUpdate` after its vertices moved.
5. **Trace**: put the top level in a heap slot with `AccelerationStructureView{tlas}`, and read
   it in HLSL as `RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[slot];`.

One-call building with memory and scratch management is a helper
([P41](../design-notes/decisions.md#p41)); compaction is not in v1.

## Signature

--8<-- "generated/signatures/RI.createAccelerationStructure.md"

--8<-- "generated/signatures/RI.cmdBuildBlas.md"

--8<-- "generated/signatures/RI.cmdBuildTlas.md"

--8<-- "generated/signatures/RI.BlasTriangles.md"

--8<-- "generated/signatures/RI.AccelerationStructureInstance.md"

## Example

```cpp
RI::cmdBuildBlas(cmd, blas, blasDesc, scratch, 0);
RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eAccelerationStructure);
RI::cmdBuildTlas(cmd, tlas, tlasDesc, scratch, 0);
RI::cmdBarrier(cmd, RI::Stage::eAccelerationStructure, RI::Stage::eCompute);
RI::cmdDispatch(cmd, groupsX, groupsY, 1); // a shader with RayQuery
```

```hlsl
RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[root.tlasSlot];
RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray);
query.Proceed();
const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
```

## How RI does it

- **Vulkan**: a buffer with `ACCELERATION_STRUCTURE_STORAGE` usage placed in your memory, a
  `VkAccelerationStructureKHR` on it, `vkCmdBuildAccelerationStructuresKHR`. The resource heap's
  mutable descriptor types include acceleration structures when ray tracing is on.
- **D3D12**: a placed buffer with `RAYTRACING_ACCELERATION_STRUCTURE`, `BuildRaytracingAccelerationStructure`,
  and an acceleration-structure SRV in the heap slot.

## Rules and errors

- Vertex, index and instance buffers need `BufferUsage::eAccelerationStructureInput`.
- **The scratch address** (the buffer's GPU address plus the offset) must be a multiple of
  `device.limits.accelerationStructureScratchAlignment`: place scratch buffers on that
  alignment (**programmer error** in validation builds otherwise).
- Builds are outside rendering sections; they count as `Stage::eAccelerationStructure`: a
  barrier between a bottom-level build and the top-level build that uses it, and from the
  top-level build to the shaders that trace.
- Building into one structure while shaders trace it is a race, as for any write.
- Everything here without `device.limits.rayTracing` is **unsupported**.

## Related decisions

- [P49](../design-notes/decisions.md#p49): acceleration structures and inline ray tracing.
