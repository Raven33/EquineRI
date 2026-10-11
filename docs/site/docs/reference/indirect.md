---
symbols: [RI::cmdExecuteIndirect, RI::cmdExecuteIndirectWithRootData, RI::IndirectKind, RI::DrawIndirectArgs, RI::DrawIndexedIndirectArgs, RI::DispatchIndirectArgs, RI::indirectStride]
---

# RI::cmdExecuteIndirect, RI::cmdExecuteIndirectWithRootData

## What it does

**Indirect execution** runs draws or dispatches whose arguments the GPU wrote into a buffer:
a culling pass decides what to draw, without a round trip to the CPU
([P48](../design-notes/decisions.md#p48)).

- **Level 1**, on every GPU: `RI::cmdExecuteIndirect(cmd, kind, args, offset, maxCount,
  count, countOffset)` executes `maxCount` records of `kind` from `args` at `offset`. With a
  `count` buffer, the GPU reads the number of records to execute (a `uint32` at
  `countOffset`, at most `maxCount`).
- **Level 2**, when `device.limits.indirectRootData` is set: `RI::cmdExecuteIndirectWithRootData`
  takes records that start with the **root data** of the pipeline set, then the arguments:
  the GPU changes root data between draws (a per-draw material or object index, for example).

`IndirectKind` and the argument structs (the same layout on both backends):

| Kind | Record arguments | With |
|---|---|---|
| `eDraw` | `DrawIndirectArgs` (vertex count, instance count, first vertex, first instance) | a graphics pipeline, in a rendering section |
| `eDrawIndexed` | `DrawIndexedIndirectArgs` (index count, instance count, first index, vertex offset, first instance) | also an index buffer |
| `eDrawMesh` | `DispatchIndirectArgs` (task or mesh groups) | a mesh pipeline |
| `eDispatch` | `DispatchIndirectArgs` (groups) | a compute pipeline, outside sections |

`RI::indirectStride(kind, rootDataSize)` gives the bytes from one record to the next: the
arguments, preceded at Level 2 by the root data rounded up to 4 bytes.

## Signature

--8<-- "generated/signatures/RI.cmdExecuteIndirect.md"

--8<-- "generated/signatures/RI.cmdExecuteIndirectWithRootData.md"

--8<-- "generated/signatures/RI.IndirectKind.md"

--8<-- "generated/signatures/RI.indirectStride.md"

## Example

A Level 2 record for an indexed draw whose shader declares `DrawRoot` as its root data:

```cpp
struct DrawRecord {
    DrawRoot root;                     // the pipeline's root data
    RI::DrawIndexedIndirectArgs args;
};
static_assert(sizeof(DrawRecord) == RI::indirectStride(RI::IndirectKind::eDrawIndexed, sizeof(DrawRoot)));
RI::cmdSetPipeline(cmd, pipeline);
RI::cmdSetIndexBuffer(cmd, indices, 0, RI::IndexFormat::eUint32);
RI::cmdExecuteIndirectWithRootData(cmd, RI::IndirectKind::eDrawIndexed, records, 0, maxDraws, &drawCount, 0);
```

## How RI does it

- **Vulkan**, Level 1: `vkCmdDraw*Indirect`, `vkCmdDraw*IndirectCount`,
  `vkCmdDrawMeshTasksIndirect(Count)EXT`, `vkCmdDispatchIndirect`. Level 2:
  `VK_EXT_device_generated_commands` (enabled on Vulkan 1.4 devices that have it): an
  indirect commands layout with a push-constant token then the draw or dispatch token, executed
  with `vkCmdExecuteGeneratedCommandsEXT` for the bound pipeline. The driver's **preprocess
  buffer** is allocated by RI per command list, kept until the list is reset (the largest is
  reused) or destroyed: an RI-owned allocation, unlike everything else in RI (recorded in P98).
- **D3D12**: `ExecuteIndirect` with command signatures RI creates and caches: the draw or
  dispatch alone (Level 1), or root constants (root parameter 0) then the draw (Level 2).
  Level 2 is always available on D3D12.

## Rules and errors

- Argument and count buffers need `BufferUsage::eIndirect`; offsets are multiples of 4, and
  the records fit in the buffer (**programmer error** in validation builds).
- Level 1 dispatches take one record and no count: Vulkan has no indirect dispatch count.
  Level 2 dispatches may take both.
- Draws inside a rendering section, dispatches outside.
- The GPU reads the records at `Stage::eIndirect`: a barrier from the work that wrote them,
  for example `cmdBarrier(cmd, Stage::eCompute, Stage::eIndirect)`.
- After a Level 2 execution, set root data again before direct draws or dispatches (Vulkan
  leaves push constants undefined).
- Level 2 without `device.limits.indirectRootData` is **unsupported**.
- Level 1 has no portable per-draw index: Vulkan has `DrawIndex`, D3D12 nothing; use Level 2
  root data, or the first-instance value as an index.

## Related decisions

- [P48](../design-notes/decisions.md#p48): the two levels and the capability query.
