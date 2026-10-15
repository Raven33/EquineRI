# Proposal: the missing graphics features (2026-10-15)

The user asked for these on 2026-10-15: cube and 3D textures, depth bias, line primitives,
geometry and tessellation shaders. This page proposes the API. Nothing is implemented until
the user approves it (signatures change).

Each part says what changes, how it maps to Vulkan and D3D12, and what stays out.

## 1. Cube textures and 3D textures

### What changes

`TextureDesc` gets a shape and a depth:

```cpp
enum class TextureShape : uint8 { e2D, e3D, eCube };

struct TextureDesc {
    TextureShape shape = TextureShape::e2D; // new
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 1;  // new: 3D textures only
    uint32 layers = 1; // cube: 6 per cube (6 = one cube, 12 = an array of two cubes)
    uint32 mips = 1;
    uint32 samples = 1;
    Format format = Format::eUndefined;
    TextureUsage usage = TextureUsage::eNone;
    std::vector<Format> viewFormats;
};
```

`TextureViewDesc` does not change. What a view is in HLSL follows from the texture and the
range it covers:

| Texture | View covers | `ReadView` in HLSL | `ReadWriteView` and render targets |
|---|---|---|---|
| Cube | whole cubes (6, 12, … layers) | `TextureCube`, `TextureCubeArray` | the faces as a 2D array |
| Cube | some faces | `Texture2D`, `Texture2DArray` | the faces as a 2D array |
| 3D | a mip | `Texture3D` | `RWTexture3D`; a render target covers depth slices |

The face order is the same on both backends: +X, −X, +Y, −Y, +Z, −Z.

### Rules (checked in validation builds)

- Cube: `width == height`, `layers` is a multiple of 6, `samples == 1`.
- 3D: `layers == 1`, `samples == 1`, not a depth format.
- 2D: `depth == 1`.
- Copies of a 3D texture copy depth slices where a 2D array copies layers: `CopyLayout`
  counts slices in `layers`.

### How it maps

- **Vulkan:** cubes are 2D images created with `CUBE_COMPATIBLE`; RI makes a cube view for
  reading and a 2D-array view for writing and rendering. 3D images are created with
  `2D_ARRAY_COMPATIBLE`, so RI can render into slices. Cube arrays need `imageCubeArray`
  (every desktop GPU has it).
- **D3D12:** cubes are 2D arrays; the read view is a `TEXTURECUBE` / `TEXTURECUBEARRAY`
  shader resource view. 3D textures are `TEXTURE3D`; render targets pick slices.

## 2. Depth bias and depth clamp

### What changes

```cpp
struct DepthBias {
    int32 constant = 0;  // in units of the smallest depth step of the format
    float slope = 0.0f;  // times the triangle's depth slope
    float clamp = 0.0f;  // the largest bias added (0: no limit)
};

struct GraphicsPipelineDesc {
    // ...
    DepthBias depthBias;     // new
    bool depthClamp = false; // new: clamp depth to 0..1 instead of clipping
};
```

Like the rest of the pipeline, the bias is fixed when the pipeline is created
(P38): shadow maps with two biases use two pipelines.

### How it maps

- **Vulkan:** `depthBiasEnable` with the three values; `depthClampEnable`. Both need features
  every desktop GPU has (`depthBiasClamp`, `depthClamp`).
- **D3D12:** `DepthBias`, `SlopeScaledDepthBias`, `DepthBiasClamp`;
  `DepthClipEnable = !depthClamp`.
- `constant` is an integer because D3D12 takes an integer; Vulkan's float is given the
  same value. Both apply the same formula.

## 3. Lines, points and wireframe

### What changes

```cpp
enum class Topology : uint8 { eTriangleList, eTriangleStrip, eLineList, eLineStrip, ePointList,
                              ePatchList }; // eLineStrip and ePatchList are new
enum class FillMode : uint8 { eSolid, eWireframe };

struct GraphicsPipelineDesc {
    // ...
    FillMode fill = FillMode::eSolid; // new: eWireframe draws triangle edges as lines
};
```

`eLineList` and `ePointList` exist already. `ePatchList` is for tessellation (part 4).

### What stays out

**Line width and point size other than one pixel.** D3D12 has neither. For thick lines or
large points, draw quads (two triangles per segment or point).

### How it maps

- **Vulkan:** `LINE_STRIP`; `polygonMode = LINE` (feature `fillModeNonSolid`, every desktop
  GPU has it).
- **D3D12:** `LINESTRIP`; `FillMode = WIREFRAME`.

## 4. Geometry and tessellation shaders

### What changes

```cpp
struct GraphicsPipelineDesc {
    Bytecode vertex;
    Bytecode hull;     // new: tessellation, with `domain` (Vulkan: tessellation control)
    Bytecode domain;   // new: (Vulkan: tessellation evaluation)
    Bytecode geometry; // new
    Bytecode pixel;
    // ... task and mesh as before ...
    uint32 patchControlPoints = 0; // new: control points per patch, with Topology::ePatchList
};

struct DeviceLimits {
    // ...
    bool geometryShaders = false; // new
    bool tessellation = false;    // new
};
```

The order of the stages is vertex → hull → domain → geometry → pixel. A pipeline uses
`hull` and `domain` together or not at all, and `geometry` optionally. Mesh pipelines take
neither. Shaders are compiled with the `hs_6_6`, `ds_6_6` and `gs_6_6` profiles.

### Rules (checked in validation builds)

- `hull` and `domain` come together, with `Topology::ePatchList` and a
  `patchControlPoints` from 1 to 32.
- Root data is unchanged: one block for the whole pipeline, which every stage sees (P44). As
  today, the only check is that every stage reading root data declares the same size.
- The values the pixel shader reads are checked against the last stage before it (P101):
  the geometry shader, else the domain shader, else the vertex shader.
- `PipelineStatisticsLayout` gains the counters of the new stages (geometry invocations and
  primitives, hull and domain invocations).

### How it maps

- **Vulkan:** features `geometryShader` and `tessellationShader` (every desktop GPU has
  them); DXC turns hull shaders into tessellation control shaders and domain shaders into
  tessellation evaluation shaders.
- **D3D12:** always available.

### Worth knowing

Geometry shaders are slow on AMD and NVIDIA. Mesh shaders (in RI already) or compute shaders
usually do the same work faster; geometry shaders are useful for ports and simple cases
(layered rendering into the six faces of a cube in one draw).

## What stays out, still

Compressed formats (BC), multiview, alpha to coverage, conservative rasterization, shading
rate, wide lines, large points, ray tracing pipelines, several queues. The glossary's
"Not in RI v1" table is updated as the features above land.
