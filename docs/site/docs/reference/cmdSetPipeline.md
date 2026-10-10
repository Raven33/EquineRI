---
symbols: [RI::cmdSetPipeline]
---

# RI::cmdSetPipeline

## What it does

`RI::cmdSetPipeline(cmd, pipeline)` chooses the pipeline that the following commands run: a
[compute pipeline](pipeline.md) for [`cmdDispatch`](cmdDispatch.md), or a
[graphics pipeline](graphicsPipeline.md) for [`cmdDraw`](cmdDraw.md) inside a
[rendering section](../concepts/rendering.md). One pipeline is current at a time on D3D12:
after a dispatch, set the graphics pipeline again before drawing, and the other way round. It also sets the pipeline's layout (its
root signature on D3D12) and remembers the shader's root data size, so
[`cmdSetRootData`](cmdSetRootData.md) can be checked against it.

You may switch pipelines as often as you like within a list. Set the root data again after a
switch: each pipeline expects its own.

## Signature

--8<-- "generated/signatures/RI.cmdSetPipeline.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:set-state"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:set-state"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:set-pipeline"
    ```

    What to watch for: binding a pipeline does not bind its layout's descriptor sets or push
    constants; those are bound separately, with a compatible layout.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:set-pipeline"
    ```

    What to watch for: the root signature and the pipeline state are set separately. Setting
    a root signature clears the root arguments (root constants), so RI sets it only when it
    changes.

## Rules and errors

- **Set the heap first**: [`cmdSetHeap`](cmdSetHeap.md) must come before `cmdSetPipeline` in
  each recording, on every backend ([P94](../design-notes/decisions.md#p94)). In validation
  builds, a pipeline set with no heap is a **programmer error**: `cmdSetPipeline: set a
  descriptor heap with cmdSetHeap first (D3D12 needs the heaps bound before a root signature
  that indexes them)`.
- In validation builds, a destroyed pipeline is a **programmer error**.
- In a read-only depth section ([P75](../design-notes/decisions.md#p75)), a pipeline that
  writes depth is a **programmer error** in validation builds: `cmdSetPipeline: the pipeline
  writes depth, but the rendering section's depth is read-only (P75)`.

## Related decisions

- [P38](../design-notes/decisions.md#p38): pipelines are created up front, never during
  recording.
- [P70](../design-notes/decisions.md#p70): RI rebinds its implicit state (heap sets) when the
  layout changes.
- [P94](../design-notes/decisions.md#p94): heap before pipeline.
