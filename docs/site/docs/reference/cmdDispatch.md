---
symbols: [RI::cmdDispatch]
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

- Each dimension is limited to 65,535 groups on D3D12, and to the GPU's
  `maxComputeWorkGroupCount` on Vulkan (at least 65,535).
- RI records the dispatch as it is; it checks nothing more here (the native validation layers
  report a missing pipeline).

## Related decisions

- [P34](../design-notes/decisions.md#p34): immediate recording.
- [P85](../design-notes/decisions.md#p85): the `cmd` prefix.
