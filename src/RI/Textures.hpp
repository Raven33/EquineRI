#pragma once

// Portable texture, view, rendering and graphics-pipeline descriptions
// (CONTEXT.md P38, P39, P40, P58, P65, P73, P74, P75, P95). The objects are
// per-backend structs (P83): RI::Vk::Texture, RI::D3D12::Texture, ...
// Image layouts never appear here (P16): each backend keeps them internal.

#include <RI/Flags.hpp>
#include <RI/Resources.hpp>
#include <types.hpp>

#include <array>
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
struct TextureViewDesc {
    Format format = Format::eUndefined; // eUndefined: the texture's format
    uint32 baseMip = 0;
    uint32 mipCount = kAllMips;
    uint32 baseLayer = 0;
    uint32 layerCount = kAllLayers;
    Aspect aspect = Aspect::eDefault;
};

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
enum class CullMode : uint8 { eNone, eFront, eBack };
enum class Topology : uint8 { eTriangleList, eTriangleStrip, eLineList, ePointList };

struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::eLess;
};

// Complete state up front (P38). No vertex input: vertices are pulled from
// buffers in the shader (P39). Blending comes later.
struct GraphicsPipelineDesc {
    Bytecode vertex; // entry point "main"
    Bytecode pixel;  // entry point "main"; may be empty for depth-only pipelines
    std::vector<Format> colorFormats;
    Format depthFormat = Format::eUndefined;
    uint32 samples = 1;
    Topology topology = Topology::eTriangleList;
    CullMode cull = CullMode::eNone;
    bool frontCounterClockwise = false;
    DepthState depth;
};

// ── Queries (P51, P71) ───────────────────────────────────────────────────────

enum class QueryType : uint8 {
    eTimestamp, // one 64-bit tick count per query; ticks per second in DeviceLimits
};

struct QueryPoolDesc {
    QueryType type = QueryType::eTimestamp;
    uint32 count = 0;
};

} // namespace RI
