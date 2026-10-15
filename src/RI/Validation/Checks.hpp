#pragma once

// RI's own rules for memory, descriptors, pipelines and command lists
// (P88: only what native layers cannot see). Plain checks returning a
// CheckResult (P91); backends call them through EQ_VALIDATE.

#include <RI/Textures.hpp>
#include <RI/Validation/Validation.hpp>
#include <types.hpp>

#include <span>
#include <string>
#include <string_view>

namespace RI::Validation {

// A descriptor slot inside its heap (P59). RI computes native addresses from
// slots, so an out-of-range slot would silently write past the heap on D3D12.
CheckResult checkSlot(uint32 slot, uint32 count, std::string_view heap);

// Root data passed to cmdSetRootData matches the size the pipeline's shader
// declares (P24, P45).
CheckResult checkRootDataSize(uint64 given, uint32 declared);

// A command list is reset, destroyed or re-recorded only after the GPU has
// finished its last submission (P56).
CheckResult checkCommandListIdle(uint64 lastSubmitted, uint64 completed, std::string_view action);

// A range [offset, offset + size) fits in `total` bytes and `offset` is a
// multiple of `alignment` (resource placement P31, buffer views P87).
CheckResult checkRange(uint64 offset, uint64 size, uint64 total, uint64 alignment, std::string_view what);

// A descriptor heap is set before a pipeline (P94): D3D12 requires the heaps
// bound before a root signature that indexes them, so RI makes it the rule on
// every backend.
CheckResult checkHeapSetBeforePipeline(bool heapSet);

// Buffer views (P87), one rule for both backends (D3D12 addresses views in
// elements and 256-byte constant blocks): read and read-write views start at a
// multiple of 16 bytes and of `stride`, and cover whole elements; constant
// buffer views start at a multiple of 256 and their size, rounded up to 256,
// fits in the buffer. `size` is already resolved (no kWholeSize).
CheckResult checkBufferView(uint64 offset, uint64 size, uint64 bufferSize, uint32 stride, bool constant);

// Rounds a constant buffer view's size up to the 256-byte block (both backends).
inline constexpr uint64 constantViewSize(uint64 size) { return (size + 255) & ~uint64{255}; }

// A command that must be recorded inside (`inside` true) or outside a
// rendering section (P40, P65, P71): `call` names it.
CheckResult checkRenderingSection(bool rendering, bool inside, std::string_view call);

// A view's format reinterprets the texture's (P74): same bytes per texel.
CheckResult checkViewFormatCompatible(Format texture, Format view);

// A view's mip and layer range (counts already resolved) lies inside the texture (P73).
CheckResult checkViewRange(uint32 baseMip, uint32 mipCount, uint32 mips, uint32 baseLayer, uint32 layerCount,
                           uint32 layers);

// A texture view used by a copy (P47): one mip, single-sampled for buffer
// copies, one aspect, and the texture has `usage` (eCopySrc or eCopyDst).
CheckResult checkCopyView(uint32 mipCount, uint32 samples, bool bufferCopy, uint32 bytesPerTexel, bool hasUsage,
                          std::string_view usage, std::string_view call);
// cmdCopyTexture: the same size, layer count, sample count and texel size.
CheckResult checkCopyTextureMatch(uint32 srcWidth, uint32 srcHeight, uint32 srcLayers, uint32 srcSamples,
                                  uint32 srcBytes, uint32 dstWidth, uint32 dstHeight, uint32 dstLayers,
                                  uint32 dstSamples, uint32 dstBytes);

// A query command used with the right kind of pool (P71): `ok` says whether
// the pool's type fits `call`.
CheckResult checkQueryKind(bool ok, std::string_view call);

// Graphics pipeline state (P38): `blend` is empty or one per color format,
// and the stencil test needs a depth format with stencil.
CheckResult checkGraphicsState(size_t colorCount, size_t blendCount, bool stencilTest, bool depthHasStencil);

// Graphics pipeline stages (P102): hull and domain shaders come together, with
// Topology::ePatchList and 1 to 32 control points; mesh pipelines take neither
// them nor a geometry shader.
CheckResult checkGraphicsStages(bool mesh, bool hull, bool domain, bool geometry, bool patchList,
                                uint32 patchControlPoints);

// One value passed from the vertex (or mesh) shader to the pixel shader (P101):
// its semantic, normalized by stageSemantic, and where the backend places it
// (Vulkan: location, component 0; D3D12: register and first component).
// `slotChecked` is false where the backend numbers the producer's slots apart
// from the pixel shader's (D3D12 per-primitive mesh outputs): then only the
// semantic is matched.
struct StageValue {
    std::string semantic;
    uint32 slot = 0;
    uint32 component = 0;
    bool slotChecked = true;
};
// A semantic as both backends compare it: upper case (HLSL semantics ignore
// case), with its index ("NORMAL" -> "NORMAL0", "color1" -> "COLOR1").
std::string stageSemantic(std::string_view semantic);
// Every value the pixel shader reads is written by the previous stage, in the
// same slot (P101). Vulkan pairs values by location and D3D12 by register, both
// numbered in declaration order, so a pixel shader that leaves out a value
// before others would read the wrong one (Vulkan, silently) or fail (D3D12).
// `producer` names the previous stage, `slotName` the backend's slot.
CheckResult checkStageLink(std::span<const StageValue> outputs, std::span<const StageValue> inputs,
                           std::string_view producer, std::string_view slotName);
// cmdSetIndexBuffer: the buffer has eIndex usage and the offset is a multiple
// of the index size.
CheckResult checkIndexBuffer(bool hasUsage, uint64 offset, uint32 indexSize, uint64 bufferSize);

// A resolve (P52, P76) from `src` into `dst`: a multisampled source, a
// single-sampled destination of the same size, layers and format, the
// destination's attachment usage, a supported mode (color: eAverage on a
// non-integer format; depth/stencil: in `depthModes`, and `stencilModes`
// when the format has stencil).
struct ResolveCheck {
    uint32 srcSamples, dstSamples;
    uint32 srcWidth, srcHeight, srcLayers;
    uint32 dstWidth, dstHeight, dstLayers;
    Format srcFormat, dstFormat;
    bool dstHasAttachmentUsage;
    ResolveMode mode;
    uint32 depthModes, stencilModes;
};
CheckResult checkResolve(const ResolveCheck &r, std::string_view call);

// An indirect execution (P48): buffers with eIndirect usage, offsets multiple
// of 4, records inside the buffer, dispatches one at a time without a count.
struct IndirectCheck {
    bool dispatch;
    bool argsUsage;
    uint64 offset, stride, argsSize;
    uint32 maxCount;
    bool hasCount, countUsage;
    uint64 countOffset, countSize;
    bool countForDispatchAllowed; // Level 2 dispatches may take a count
};
CheckResult checkIndirect(const IndirectCheck &c, std::string_view call);
// Vulkan: the memory's native type is one the resource allows (allocate it
// with allocateMemory(kind, size, &requirements) when the kind's default type
// does not fit).
CheckResult checkMemoryType(bool allowed, std::string_view call);
// A sampler description: anisotropy 1 to 16, minLod <= maxLod, min/max
// reduction only where supported and not with a comparison.
CheckResult checkSampler(const SamplerDesc &desc, bool minMaxSupported);
// The scratch range of cmdExecuteIndirectWithRootData (P48).
struct IndirectScratchCheck {
    uint64 requiredSize, requiredAlignment;
    bool hasScratch, usage;
    uint64 offset, scratchSize;
    uint64 address; // GPU address of the range, for the alignment
};
CheckResult checkIndirectScratch(const IndirectScratchCheck &c, std::string_view call);

// cmdEndMarker closes an open marker region (P53); `depth` is the open count.
CheckResult checkMarkerEnd(uint32 depth);
// cmdEnd: every marker region opened in the command list is closed.
CheckResult checkMarkersClosed(uint32 depth);

// eReadOnlyDepthSampling (P97) goes with eSampled, eDepthStencil and a depth format.
CheckResult checkTextureUsage(Format format, TextureUsage usage);

} // namespace RI::Validation
