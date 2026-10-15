---
symbols: [RI::createGraphicsPipeline, RI::GraphicsPipelineDesc, RI::DepthState, RI::CompareOp, RI::CullMode, RI::Topology, RI::FillMode, RI::DepthBias, RI::StencilState, RI::StencilFace, RI::StencilOp, RI::BlendState, RI::BlendFactor, RI::BlendOp, RI::ColorMask]
---

# RI::createGraphicsPipeline

## What it does

`RI::createGraphicsPipeline(device, desc)` compiles a vertex shader, optional tessellation and
geometry shaders, an optional pixel shader and **all** the fixed-function state into a
pipeline, at once
([P38](../design-notes/decisions.md#p38)). RI never compiles a pipeline later, in the middle of
recording: what you create is what runs. Set it with [`cmdSetPipeline`](cmdSetPipeline.md) inside
a [rendering section](../concepts/rendering.md), and destroy it with
[`destroyPipeline`](pipeline.md).

`GraphicsPipelineDesc`:

- `vertex`, `pixel`: bytecode (SPIR-V or DXIL), entry point `main`. `pixel` may be empty for a
  depth-only pipeline;
- `hull`, `domain`: **tessellation** ([P102](../design-notes/decisions.md#p102)), both or
  neither, compiled with profiles `hs_6_6` and `ds_6_6` (Vulkan calls them tessellation control
  and evaluation shaders). They need `topology = ePatchList`, `patchControlPoints` from 1 to 32,
  and `device.limits.tessellation`;
- `geometry`: a **geometry shader** (`gs_6_6`), after the vertex or domain shader. It needs
  `device.limits.geometryShaders`. Geometry shaders are slow on AMD and NVIDIA: mesh or
  compute shaders usually do the same work faster. They are handy for ports and for layered
  rendering, such as the six faces of a [cube texture](texture.md#shapes) in one draw
  (`SV_RenderTargetArrayIndex`);
- `task`, `mesh`: a **mesh pipeline** gives a mesh shader instead of `vertex` (compiled with
  profile `ms_6_6`), and optionally a task (amplification) shader before it (`as_6_6`). It is
  drawn with [`cmdDrawMesh`](cmdDraw.md); `topology` does not apply (the mesh shader declares
  its own). Mesh shaders need `device.limits.meshShaders`;
- `colorFormats`, `depthFormat`, `samples`: the formats and sample count of the attachments it
  renders to. They must match the rendering section's;
- `topology`: `eTriangleList` (default), `eTriangleStrip`, `eLineList`, `eLineStrip`,
  `ePointList`, `ePatchList` (tessellation). Points are one pixel and lines one pixel wide on
  both backends: D3D12 has no point size or line width. For larger points (sprites) or thick
  lines, draw quads;
- `fill`: `eSolid` (default) or `eWireframe`, which draws the edges of triangles as lines;
- `cull`: `eNone` (default), `eFront`, `eBack`; `frontCounterClockwise`: which winding faces
  the camera (clockwise by default, the same on both backends);
- `depth`: `test` (compare against the depth attachment), `write` (store the new depth),
  `compare` (`eLess` by default);
- `depthBias`: added to each fragment's depth before the test, against shadow acne:
  `constant` times the smallest depth step of the format, plus `slope` times the triangle's
  depth slope, at most `clamp` in size (0: no limit). `constant` is an integer because D3D12
  takes one. Like all state it is fixed: two biases are two pipelines;
- `depthClamp`: clamp depth to 0..1 instead of clipping triangles beyond the near and far
  planes (shadow maps of objects behind the light's near plane);
- `stencil`: `test`, one `readMask` and one `writeMask` for both faces (as D3D12 has), the
  `reference` value, and per face (`front`, `back`) the operations on `fail`, `depthFail` and
  `pass` and the `compare` function. The reference is the pipeline's default:
  [`cmdSetStencilReference`](cmdSetViewport.md) overrides it inside a section, so one pipeline
  can tag objects with different values ([P104](../design-notes/decisions.md#p104));
- `blend`: one `BlendState` per color format, or none (every target written as is). Each has
  `enable`, the color and alpha factors and operations (`result = src * srcFactor (op)
  dst * dstFactor`; `eMin` and `eMax` ignore the factors) and a `writeMask` of channels.

There is **no vertex input** ([P39](../design-notes/decisions.md#p39)): vertex shaders pull
their data from buffers by `SV_VertexID` (indexed draws give the index as `SV_VertexID`, see
[`cmdDrawIndexed`](cmdDraw.md)).

Stages run vertex → hull → domain → geometry → pixel. The root data is the one
`EQ_ROOT_DATA` block every stage sees; `pipeline.rootDataSize` reports its size
([P44](../design-notes/decisions.md#p44)).

## Signature

--8<-- "generated/signatures/RI.createGraphicsPipeline.md"

--8<-- "generated/signatures/RI.GraphicsPipelineDesc.md"

--8<-- "generated/signatures/RI.DepthState.md"

--8<-- "generated/signatures/RI.CompareOp.md"

--8<-- "generated/signatures/RI.CullMode.md"

--8<-- "generated/signatures/RI.Topology.md"

--8<-- "generated/signatures/RI.FillMode.md"

--8<-- "generated/signatures/RI.DepthBias.md"

--8<-- "generated/signatures/RI.StencilState.md"

--8<-- "generated/signatures/RI.StencilFace.md"

--8<-- "generated/signatures/RI.BlendState.md"

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
    (triangle, line, point, patch): the exact topology, with the number of control points of
    a patch, is set when drawing. Every member left at zero
    must still be valid (`SampleMask`, the blend description).

## Rules and errors

- Every stage that declares root data must declare the same size: a different size is a
  **programmer error**. A stage that declares root data but never reads it counts as none
  ([root data](../concepts/root-data.md#one-c-struct-for-both-backends)). Root data larger than `device.limits.maxRootDataSize` is **unsupported**.
- At most 8 color formats (**programmer error** on D3D12).
- Every value the pixel shader reads is written by the stage before it (geometry, domain,
  vertex or mesh shader) in the same place: pass them in one struct ([values passed to the pixel shader](../concepts/shaders.md#values-passed-to-the-pixel-shader),
  [P101](../design-notes/decisions.md#p101); **programmer error** in validation builds).
- Shader bytecode that is not SPIR-V on Vulkan (or DXIL on D3D12) is a **programmer error**.
- To write depth, set both `test` and `write`; to write depth unconditionally, compare with
  `CompareOp::eAlways`. (Vulkan writes depth only when the test is on.)
- A pipeline with `depth.write` set is refused in a read-only depth section (validation builds).
- Exactly one of `vertex` and `mesh`; `task` only with `mesh` (**programmer error**). A mesh
  pipeline on a GPU without mesh shaders is **unsupported**.
- `hull` and `domain` together, with `ePatchList` and 1 to 32 `patchControlPoints`; `ePatchList`
  only with them; no `hull`, `domain` or `geometry` in a mesh pipeline (**programmer error** in
  validation builds). Geometry or tessellation on a GPU without them is **unsupported**.
- `blend` holds no states or one per color format; the stencil test needs a `depthFormat` with
  stencil (**programmer error** in validation builds).
- Alpha blend factors that name a color (`eSrcColor`, ...) are read as the matching alpha
  factor, as D3D12 requires.
- `samples` must be supported for every attachment format ([`formatSupported`](formatSupported.md)).

## Related decisions

- [P38](../design-notes/decisions.md#p38): complete state up front, compiled at creation.
- [P39](../design-notes/decisions.md#p39): vertex pulling, no input layouts.
- [P44](../design-notes/decisions.md#p44), [P45](../design-notes/decisions.md#p45): root data
  from the shader.
- [P67](../design-notes/decisions.md#p67): RI builds the D3D12 root signature.
- [P102](../design-notes/decisions.md#p102): geometry and tessellation shaders, line strips,
  wireframe, depth bias and clamp.
