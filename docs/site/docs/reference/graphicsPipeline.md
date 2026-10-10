---
symbols: [RI::createGraphicsPipeline, RI::GraphicsPipelineDesc, RI::DepthState, RI::CompareOp, RI::CullMode, RI::Topology]
---

# RI::createGraphicsPipeline

## What it does

`RI::createGraphicsPipeline(device, desc)` compiles a vertex shader, an optional pixel shader
and **all** the fixed-function state into a pipeline, at once
([P38](../design-notes/decisions.md#p38)). RI never compiles a pipeline later, in the middle of
recording: what you create is what runs. Set it with [`cmdSetPipeline`](cmdSetPipeline.md) inside
a [rendering section](../concepts/rendering.md), and destroy it with
[`destroyPipeline`](pipeline.md).

`GraphicsPipelineDesc`:

- `vertex`, `pixel`: bytecode (SPIR-V or DXIL), entry point `main`. `pixel` may be empty for a
  depth-only pipeline;
- `colorFormats`, `depthFormat`, `samples`: the formats and sample count of the attachments it
  renders to. They must match the rendering section's;
- `topology`: `eTriangleList` (default), `eTriangleStrip`, `eLineList`, `ePointList`;
- `cull`: `eNone` (default), `eFront`, `eBack`; `frontCounterClockwise`: which winding faces
  the camera (clockwise by default, the same on both backends);
- `depth`: `test` (compare against the depth attachment), `write` (store the new depth),
  `compare` (`eLess` by default).

There is **no vertex input** ([P39](../design-notes/decisions.md#p39)): vertex shaders pull
their data from buffers by `SV_VertexID`. Blending comes later: every color target is written
as is.

The root data is the one `EQ_ROOT_DATA` block both stages declare; `pipeline.rootDataSize`
reports its size ([P44](../design-notes/decisions.md#p44)).

## Signature

--8<-- "generated/signatures/RI.createGraphicsPipeline.md"

--8<-- "generated/signatures/RI.GraphicsPipelineDesc.md"

--8<-- "generated/signatures/RI.DepthState.md"

--8<-- "generated/signatures/RI.CompareOp.md"

--8<-- "generated/signatures/RI.CullMode.md"

--8<-- "generated/signatures/RI.Topology.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:create-pipeline"
```

A depth-only pipeline (a depth prepass, a shadow map):

```cpp
--8<-- "snippets/ri/textures.cpp:create-pipeline-depth-only"
```

The shaders:

```hlsl
--8<-- "snippets/hlsl/doc_triangle_vs.hlsl:main"
```

```hlsl
--8<-- "snippets/hlsl/doc_triangle_ps.hlsl:main"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:create-pipeline"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:create-pipeline"
    ```

    What to watch for: with dynamic rendering, `VkPipelineRenderingCreateInfo` gives the
    attachment formats instead of a render pass. Viewport and scissor are dynamic state, so
    one pipeline fits any target size. The pipeline layout must match the descriptor sets
    and push constants the shaders use.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:create-pipeline"
    ```

    What to watch for: the root signature is part of the pipeline; RI builds it from the DXIL
    ([P67](../design-notes/decisions.md#p67)). `PrimitiveTopologyType` is only the family
    (triangle, line, point): the exact topology is set when drawing. Every member left at zero
    must still be valid (`SampleMask`, the blend description).

## Rules and errors

- Both stages must declare the same root data, or none: a different size is a **programmer
  error**. Root data larger than `device.limits.maxRootDataSize` is **unsupported**.
- At most 8 color formats (**programmer error** on D3D12).
- Shader bytecode that is not SPIR-V on Vulkan (or DXIL on D3D12) is a **programmer error**.
- To write depth, set both `test` and `write`; to write depth unconditionally, compare with
  `CompareOp::eAlways`. (Vulkan writes depth only when the test is on.)
- A pipeline with `depth.write` set is refused in a read-only depth section (validation builds).
- `samples` must be supported for every attachment format ([`formatSupported`](formatSupported.md)).

## Related decisions

- [P38](../design-notes/decisions.md#p38): complete state up front, compiled at creation.
- [P39](../design-notes/decisions.md#p39): vertex pulling, no input layouts.
- [P44](../design-notes/decisions.md#p44), [P45](../design-notes/decisions.md#p45): root data
  from the shader.
- [P67](../design-notes/decisions.md#p67): RI builds the D3D12 root signature.
