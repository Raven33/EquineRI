---
symbols: [RI::cmdBarrier, RI::Stage]
---

# RI::cmdBarrier

## What it does

`RI::cmdBarrier(cmd, before, after)` makes **every write** of the `before` stages, in commands
recorded earlier, finished and visible to **every access** of the `after` stages, in commands
recorded later ([P36](../design-notes/decisions.md#p36), [P94](../design-notes/decisions.md#p94)).

Use it between a command that writes memory and a later one that reads (or writes) it: a copy
then a dispatch that reads the copied data, a dispatch then a copy of its output, two
dispatches where the second reads what the first wrote, a rendering section then a shader that
samples its result. Why barriers exist, and why RI's are coarse, is explained in
[Barriers](../concepts/barriers.md).

| `Stage` | Covers |
|---|---|
| `eCopy` | copies and clears |
| `eCompute` | compute shaders |
| `eIndirect` | reading indirect arguments ([`cmdExecuteIndirect`](indirect.md)) |
| `eVertex` | vertex shaders |
| `ePixel` | pixel shaders |
| `eRenderTarget` | color attachment reads and writes |
| `eDepthStencil` | depth and stencil tests and writes |
| `eAll` | every stage |

A barrier names no texture and changes no layout: textures stay in their resting layout between
operations ([Image layouts are RI's business](../concepts/layouts.md)).

## Signature

--8<-- "generated/signatures/RI.cmdBarrier.md"

--8<-- "generated/signatures/RI.Stage.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:barrier-copy-compute"
```

```cpp
--8<-- "snippets/ri/compute.cpp:barrier-compute-copy"
```

After a rendering section, before shaders sample what it wrote:

```cpp
--8<-- "snippets/ri/textures.cpp:barrier-render-sample"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:barrier-copy-compute"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:barrier-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:barrier-copy-compute"
    ```

    What to watch for: a global `VkMemoryBarrier2` (synchronization2) needs no buffer or
    image. Stage *and* access masks both matter: the stages order the work, the accesses make
    the memory visible.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:barrier-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:barrier-copy-compute"
    ```

    What to watch for: enhanced barriers (`D3D12_GLOBAL_BARRIER`) replace resource-state
    transitions for buffers; the sync and access values must be compatible with each other.

## Rules and errors

- Two reads never need a barrier between them.
- Not inside a rendering section: end the section, barrier, begin the next.
- Attachment stages (`eRenderTarget`, `eDepthStencil`): on D3D12 global barriers cannot name
  attachment accesses, so these stages add nothing there and RI's switch back to rest at
  `cmdEndRendering` makes the attachment writes visible. Write the barrier anyway: Vulkan uses
  it.
- `Stage::eAll` waits for, and blocks, everything: correct but slower than naming the stages.
- No barrier is needed before the CPU reads results: [`cmdEnd`](cmdBegin.md) and the timeline
  point handle it.
- A barrier also covers earlier **submissions**: when a command list reads what a previous
  submission wrote, start it with a barrier (for example `cmdBarrier(cmd, Stage::eAll,
  Stage::eCompute)`). Do not count on submission order alone: Vulkan does not wait between
  submissions unless told to.

## Related decisions

- [P36](../design-notes/decisions.md#p36): explicit, coarse stage to stage barriers.
- [P72](../design-notes/decisions.md#p72): no resource-naming barrier.
- [P10](../design-notes/decisions.md#p10): coarse is the default.
- [P94](../design-notes/decisions.md#p94): the stages and their native mapping.
- [P68](../design-notes/decisions.md#p68): RI's own switches around its operations.
