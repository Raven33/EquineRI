---
symbols: [RI::cmdDraw]
---

# RI::cmdDraw

## What it does

`RI::cmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance)` draws
`vertexCount` vertices, `instanceCount` times (1 by default), with the graphics pipeline set by
[`cmdSetPipeline`](cmdSetPipeline.md) and the root data set by
[`cmdSetRootData`](cmdSetRootData.md), into the attachments of the open
[rendering section](../concepts/rendering.md).

There are no vertex buffers ([P39](../design-notes/decisions.md#p39)). The vertex shader
receives `SV_VertexID` (from `firstVertex` to `firstVertex + vertexCount - 1`) and
`SV_InstanceID`, and fetches its data itself, from buffers in the descriptor heap: **vertex
pulling**. A full-screen triangle needs no data at all:

```hlsl
--8<-- "snippets/hlsl/doc_triangle_vs.hlsl:main"
```

## Signature

--8<-- "generated/signatures/RI.cmdDraw.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:draw"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:draw"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:draw"
    ```

    What to watch for: the graphics bind point has its own pipeline and descriptor sets,
    separate from compute. Without vertex input, the pipeline's vertex input state is empty.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:draw"
    ```

    What to watch for: the graphics root signature and root constants are separate from
    compute (`SetGraphicsRoot*`). The exact topology is command-list state
    (`IASetPrimitiveTopology`); RI sets it from the pipeline at `cmdSetPipeline`.

## Rules and errors

- Inside a rendering section only, with a graphics pipeline set. On Vulkan the validation
  layer reports a draw outside a section (RI does not check, so native sections may hold RI
  draws).
- The pipeline's attachment formats and sample count must match the section's.
- Indexed and indirect draws come later.

## Related decisions

- [P39](../design-notes/decisions.md#p39): vertex pulling, no vertex input.
- [P40](../design-notes/decisions.md#p40): rendering sections.
- [P38](../design-notes/decisions.md#p38): complete pipeline state up front.
