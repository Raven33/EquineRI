---
symbols: [RI::cmdSetHeap]
---

# RI::cmdSetHeap

## What it does

`RI::cmdSetHeap(cmd, heap)` makes a [descriptor heap](descriptorHeap.md) the one that shaders
index (`ResourceDescriptorHeap[slot]`) in the following dispatches of this command list.

Set it **once per recording, before the first [`cmdSetPipeline`](cmdSetPipeline.md)**. The
heap stays set when you change pipelines: RI rebinds whatever the backend needs
([P70](../design-notes/decisions.md#p70): RI keeps its own implicit state).

## Signature

--8<-- "generated/signatures/RI.cmdSetHeap.md"

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
    --8<-- "snippets/vulkan/compute.cpp:set-heap"
    ```

    What to watch for: descriptor sets are bound *with a pipeline layout*. Layouts that
    differ in push-constant size are not compatible, so RI binds the heap's sets again
    whenever a pipeline with another layout is set.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:set-heap"
    ```

    What to watch for: `SetDescriptorHeaps` takes both heaps (views and samplers) in one
    call, and must come before `SetComputeRootSignature` for a root signature that indexes
    the heaps directly.

## Rules and errors

- **The heap comes before the pipeline**, on every backend. D3D12 requires the descriptor
  heaps bound before a root signature with
  `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`, so RI makes it the rule
  everywhere ([P94](../design-notes/decisions.md#p94)). In validation builds,
  `cmdSetPipeline` without a heap set is a **programmer error**: `cmdSetPipeline: set a
  descriptor heap with cmdSetHeap first`.
- Setting another heap later in the same list is allowed; RI sets the current root
  signature (D3D12) or descriptor sets (Vulkan) again.
- In validation builds, a destroyed heap is a **programmer error**.

## Related decisions

- [P59](../design-notes/decisions.md#p59): descriptor heaps.
- [P70](../design-notes/decisions.md#p70): RI restores its implicit state.
- [P94](../design-notes/decisions.md#p94): heap before pipeline (10); the Vulkan heap sets.
