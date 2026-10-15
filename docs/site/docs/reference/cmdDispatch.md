---
symbols: [RI::cmdDispatch, RI::kMaxGroupsPerDimension]
---

# RI::cmdDispatch

## What it does

`RI::cmdDispatch(cmd, x, y, z)` runs the current compute [pipeline](cmdSetPipeline.md) on a
grid of `x * y * z` **thread groups**. Each group has the size the shader declares with
`[numthreads(...)]`, so the total number of threads is the product of both.

To cover `n` items with groups of 64, round up: `(n + 63) / 64` groups. Threads past the end
should do nothing; the shader checks its index against the count:

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:main"
```

Before dispatching, set the heap, the pipeline and the root data. If the shader reads what an
earlier command wrote, put a [barrier](cmdBarrier.md) in between.

## Signature

--8<-- "generated/signatures/RI.cmdDispatch.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:dispatch"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:dispatch"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:dispatch"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:dispatch"
    ```

## Rules and errors

- Each dimension is at most `kMaxGroupsPerDimension` (65,535) groups on **every** backend
  ([P106](../design-notes/decisions.md#p106)): D3D12's limit, which Vulkan guarantees as a
  minimum. Vulkan GPUs often accept more in `x`; RI still refuses it, so a program tested on
  Vulkan never breaks on D3D12. A larger dispatch is a **programmer error** in validation builds:
  `cmdDispatch: 65536 x 1 x 1 groups; each dimension is at most 65535 on every backend`.
  Fold large counts into rows with the [`groupGrid`](../helpers/grid.md) helper.
- Indirect arguments ([`cmdExecuteIndirect`](indirect.md)) follow the same limit; RI cannot
  check them on the CPU.
- RI records the dispatch as it is; it checks nothing more here (the native validation layers
  report a missing pipeline).

## Related decisions

- [P34](../design-notes/decisions.md#p34): immediate recording.
- [P85](../design-notes/decisions.md#p85): the `cmd` prefix.
- [P106](../design-notes/decisions.md#p106): group counts, one rule on every backend.
