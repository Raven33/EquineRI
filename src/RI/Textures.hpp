#pragma once

// Portable texture, view, rendering and graphics-pipeline descriptions
// (CONTEXT.md P38, P39, P40, P58, P65, P73, P74, P75, P95). The objects are
// per-backend structs (P83): RI::Vk::Texture, RI::D3D12::Texture, ...
// Image layouts never appear here (P16): each backend keeps them internal.

#include <RI/Flags.hpp>
#include <RI/Resources.hpp>
#include <types.hpp>

#include <array>
#include <optional>
#include <vector>

namespace RI {

enum class Format : uint16 {
    eUndefined,
    // Color
    eR8Unorm,
    eRG8Unorm,
    eRGBA8Unorm,
    eRGBA8Srgb,
    eBGRA8Unorm,
    eBGRA8Srgb,
    eRGB10A2Unorm,
    eR11G11B10Float,
    eR16Float,
    eRG16Float,
    eRGBA16Float,
    eR32Float,
    eRG32Float,
    eRGBA32Float,
    eR32Uint,
    eRG32Uint,
    eRGBA32Uint,
    // Depth and stencil
    eD16Unorm,
    eD32Float,
    eD24UnormS8Uint, // not on AMD Vulkan: check formatSupported (P58 (2))
    eD32FloatS8Uint,
};

bool isDepthFormat(Format format);
bool hasStencil(Format format);
uint32 bytesPerTexel(Format format); // 0 for depth-stencil formats


// What a texture may be used for (P58). Also decides, inside each backend, the
// layout the texture rests in between operations (L1, A').
enum class TextureUsage : uint32 {
    eNone = 0,
    eSampled = rvl::eBit(0),      // ReadView: Texture2D<...> reads
    eStorage = rvl::eBit(1),      // ReadWriteView: RWTexture2D<...>
    eRenderTarget = rvl::eBit(2), // color attachment
    eDepthStencil = rvl::eBit(3), // depth/stencil attachment
    eCopySrc = rvl::eBit(4),
    eCopyDst = rvl::eBit(5),
    // Sampled by shaders while it is the read-only depth attachment of a
    // rendering section (P75, P97). Needs eSampled and eDepthStencil. Without
    // it, sampled depth rests ready for fast sampling, and shaders must not
    // sample it during a read-only depth section.
    eReadOnlyDepthSampling = rvl::eBit(6),
};
makeBitmask(TextureUsage)

// A 2D texture (or 2D array). 3D and cube textures come later.
struct TextureDesc {
    uint32 width = 1;
    uint32 height = 1;
    uint32 layers = 1;
    uint32 mips = 1;
    uint32 samples = 1; // MSAA sample count: 1, 2, 4, 8
    Format format = Format::eUndefined;
    TextureUsage usage = TextureUsage::eNone;
    // Other formats views may use (P74). Empty: views use `format` only, which
    // lets drivers keep compression.
    std::vector<Format> viewFormats;
};

enum class Aspect : uint8 {
    eDefault, // color for color formats, depth (and stencil) for depth formats
    eColor,
    eDepth,
    eStencil,
};

inline constexpr uint32 kAllMips = ~0u;
inline constexpr uint32 kAllLayers = ~0u;

// Part of a texture (P73): a mip range, a layer range, an aspect, a format.
// Bytes per texel as copies read and write one aspect (P47): the color size,
// 4 for depth (2 for eD16Unorm; 24-bit depth is padded to 4), 1 for stencil.
// eDefault means color, or depth for depth formats.
uint32 copyBytesPerTexel(Format format, Aspect aspect);

struct TextureViewDesc {
    Format format = Format::eUndefined; // eUndefined: the texture's format
    uint32 baseMip = 0;
    uint32 mipCount = kAllMips;
    uint32 baseLayer = 0;
    uint32 layerCount = kAllLayers;
    Aspect aspect = Aspect::eDefault;
};

// MSAA resolve (P52, P76): how the samples of a pixel become one value.
// Color resolves average; depth and stencil take the modes the device reports
// in DeviceLimits::depthResolveModes / stencilResolveModes. No emulation.
enum class ResolveMode : uint8 { eAverage, eMin, eMax, eSampleZero };
// The bit of `mode` in DeviceLimits::depthResolveModes and stencilResolveModes.
constexpr uint32 resolveModeBit(ResolveMode mode) { return 1u << static_cast<uint32>(mode); }

// Load and store operations of rendering attachments (P40).
enum class LoadOp : uint8 { eLoad, eClear, eDontCare };
enum class StoreOp : uint8 { eStore, eDontCare };

// Clear values: color for color textures, depth and stencil otherwise.
struct ClearValue {
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
    float depth = 1.0f;
    uint8 stencil = 0;
};

// ── Graphics pipelines (P38, P39) ────────────────────────────────────────────

enum class CompareOp : uint8 { eNever, eLess, eEqual, eLessEqual, eGreater, eNotEqual, eGreaterEqual, eAlways };

// Samplers (P87): descriptions written straight into the sampler heap with
// writeSamplers; shaders read them as SamplerDescriptorHeap[slot].
enum class Filter : uint8 { eNearest, eLinear };
enum class AddressMode : uint8 { eRepeat, eMirror, eClamp, eBorder };
enum class BorderColor : uint8 { eTransparentBlack, eOpaqueBlack, eOpaqueWhite };
// How the filtered texels combine: eMin and eMax need DeviceLimits::samplerMinMax.
enum class Reduction : uint8 { eAverage, eMin, eMax };

struct SamplerDesc {
    Filter minFilter = Filter::eLinear;
    Filter magFilter = Filter::eLinear;
    Filter mipFilter = Filter::eLinear;
    AddressMode addressU = AddressMode::eRepeat;
    AddressMode addressV = AddressMode::eRepeat;
    AddressMode addressW = AddressMode::eRepeat;
    float mipLodBias = 0.0f;
    float minLod = 0.0f;
    float maxLod = 1000.0f;           // all mips
    uint32 maxAnisotropy = 1;         // 1: off, up to 16; on, filtering is linear (mips too)
    std::optional<CompareOp> compare; // a comparison sampler (SamplerComparisonState)
    BorderColor border = BorderColor::eTransparentBlack;
    Reduction reduction = Reduction::eAverage;
};

struct SamplerWrite {
    uint32 slot = 0; // in the sampler heap
    SamplerDesc sampler;
};
enum class CullMode : uint8 { eNone, eFront, eBack };
// ePatchList: tessellation patches of GraphicsPipelineDesc::patchControlPoints
// control points, for pipelines with hull and domain shaders.
enum class Topology : uint8 { eTriangleList, eTriangleStrip, eLineList, eLineStrip, ePointList, ePatchList };
// eWireframe draws the edges of triangles as lines. Lines and points are one
// pixel wide on every backend.
enum class FillMode : uint8 { eSolid, eWireframe };

struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::eLess;
};

// Added to each fragment's depth before the test:
// constant * r + slope * (the triangle's depth slope), at most `clamp` in
// size (0: no limit), where r is the smallest depth step of the format.
struct DepthBias {
    int32 constant = 0; // an integer, as D3D12 takes it
    float slope = 0.0f;
    float clamp = 0.0f;
};

enum class StencilOp : uint8 {
    eKeep,
    eZero,
    eReplace, // the reference value
    eIncrementClamp,
    eDecrementClamp,
    eInvert,
    eIncrementWrap,
    eDecrementWrap,
};

struct StencilFace {
    StencilOp fail = StencilOp::eKeep;      // the stencil test fails
    StencilOp depthFail = StencilOp::eKeep; // stencil passes, depth fails
    StencilOp pass = StencilOp::eKeep;      // both pass
    CompareOp compare = CompareOp::eAlways;
};

// The stencil test (one read and one write mask for both faces, as D3D12
// has). The reference value is part of the pipeline (P38).
struct StencilState {
    bool test = false;
    uint8 readMask = 0xff;
    uint8 writeMask = 0xff;
    uint8 reference = 0;
    StencilFace front;
    StencilFace back;
};

enum class BlendFactor : uint8 {
    eZero,
    eOne,
    eSrcColor,
    eOneMinusSrcColor,
    eDstColor,
    eOneMinusDstColor,
    eSrcAlpha,
    eOneMinusSrcAlpha,
    eDstAlpha,
    eOneMinusDstAlpha,
};

enum class BlendOp : uint8 { eAdd, eSubtract, eReverseSubtract, eMin, eMax };

enum class ColorMask : uint8 {
    eNone = 0,
    eR = rvl::eBit(0),
    eG = rvl::eBit(1),
    eB = rvl::eBit(2),
    eA = rvl::eBit(3),
    eAll = 0xf,
};
makeBitmask(ColorMask)

// Blending of one color target: result = src * srcFactor (op) dst * dstFactor,
// for color and alpha separately. eMin and eMax ignore the factors.
struct BlendState {
    bool enable = false;
    BlendFactor srcColor = BlendFactor::eOne;
    BlendFactor dstColor = BlendFactor::eZero;
    BlendOp colorOp = BlendOp::eAdd;
    BlendFactor srcAlpha = BlendFactor::eOne;
    BlendFactor dstAlpha = BlendFactor::eZero;
    BlendOp alphaOp = BlendOp::eAdd;
    ColorMask writeMask = ColorMask::eAll;
};

// Complete state up front (P38). No vertex input: vertices are pulled from
// buffers in the shader (P39), or produced by mesh shaders.
// Stages run vertex -> hull -> domain -> geometry -> pixel.
struct GraphicsPipelineDesc {
    Bytecode vertex; // entry point "main"; empty for mesh pipelines
    // Tessellation (DeviceLimits::tessellation): hull and domain shaders
    // together, with Topology::ePatchList. Vulkan: tessellation control and
    // evaluation shaders.
    Bytecode hull;
    Bytecode domain;
    Bytecode geometry; // DeviceLimits::geometryShaders
    Bytecode pixel;  // entry point "main"; may be empty for depth-only pipelines
    // Mesh pipelines (P39, DeviceLimits::meshShaders): a mesh shader instead
    // of `vertex`, and an optional task (amplification) shader before it.
    // Drawn with cmdDrawMesh; `topology` is the mesh shader's.
    Bytecode task;
    Bytecode mesh;
    std::vector<Format> colorFormats;
    Format depthFormat = Format::eUndefined;
    uint32 samples = 1;
    Topology topology = Topology::eTriangleList;
    uint32 patchControlPoints = 0; // 1..32 with Topology::ePatchList
    FillMode fill = FillMode::eSolid;
    CullMode cull = CullMode::eNone;
    bool frontCounterClockwise = false;
    DepthState depth;
    DepthBias depthBias;
    bool depthClamp = false; // clamp depth to 0..1 instead of clipping
    StencilState stencil; // needs a depth format with stencil
    // One per color format, or empty: every target written as is.
    std::vector<BlendState> blend;
};

// Index buffers (P39): 16- or 32-bit indices.
enum class IndexFormat : uint8 { eUint16, eUint32 };

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

enum class QueryType : uint8 {
    eTimestamp,          // one 64-bit tick count per query; ticks per second in DeviceLimits
    eOcclusion,          // one 64-bit count of samples that passed the depth and stencil tests
    ePipelineStatistics, // counters per query, laid out as pipelineStatisticsLayout says (P71 (2))
};

// Where each counter of a pipeline-statistics result is (P71 (2)): byte
// offsets of 64-bit values inside one query's result of `size` bytes. The
// layouts differ per backend; RI reports them instead of converting. A
// counter of a stage the GPU lacks (DeviceLimits) is kNoCounter.
inline constexpr uint32 kNoCounter = ~0u;
struct PipelineStatisticsLayout {
    uint32 size = 0;                     // bytes per query result
    uint32 inputVertices = 0;            // vertices read by draws
    uint32 inputPrimitives = 0;          // primitives assembled
    uint32 vertexShaderInvocations = 0;
    uint32 clippingInvocations = 0;      // primitives entering clipping
    uint32 clippingPrimitives = 0;       // primitives leaving clipping
    uint32 pixelShaderInvocations = 0;
    uint32 computeShaderInvocations = 0;
    uint32 geometryShaderInvocations = 0;
    uint32 geometryShaderPrimitives = 0;
    uint32 hullShaderInvocations = 0;   // Vulkan: tessellation control patches
    uint32 domainShaderInvocations = 0; // Vulkan: tessellation evaluation invocations
};

// The staging layout of a texture view's texels in a buffer (P47), for
// cmdCopyBufferToTexture and cmdCopyTextureToBuffer. The view covers one mip.
// Row `y` of layer `l` starts at offset + l * layerPitch + y * rowPitch.
struct CopyLayout {
    uint32 width = 0;        // texels per row
    uint32 height = 0;       // rows per layer
    uint32 layers = 1;
    uint32 bytesPerTexel = 0;
    uint64 rowPitch = 0;     // bytes from one row to the next (>= width * bytesPerTexel)
    uint64 layerPitch = 0;   // bytes from one layer to the next
    uint64 size = 0;         // bytes the copy reads or writes from the offset
    uint64 alignment = 0;    // the buffer offset must be a multiple of this
};

struct QueryPoolDesc {
    QueryType type = QueryType::eTimestamp;
    uint32 count = 0;
};

} // namespace RI
