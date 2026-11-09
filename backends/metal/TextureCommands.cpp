// Texture copies, clears and discards on Metal (P47, P65, P69, P102, P108).
//
// Owns: copyLayout, cmdCopyBufferToTexture, cmdCopyTextureToBuffer,
// cmdCopyTexture, cmdClearTexture and cmdDiscard.
//
// Watch out: copies are blits on the open compute encoder (Metal 4's compute
// encoder blits), so they count as Stage::eCopy with no extra work. They name
// the whole texture with absolute slices and levels: the bytes are the same
// in every view format. copyLayout keeps D3D12's rule (rows on 256 bytes,
// layers on 512), which Metal accepts (it needs only whole texels), so a
// buffer laid out on Metal also suits D3D12. Metal has no clear command for
// textures outside render passes: render targets and depth are cleared by a
// render pass of one load action per mip and layer (runClearPass orders it
// like a copy); storage textures by a blit of one row from command memory,
// doubled inside the texture. Texture-to-texture copies between different
// pixel formats, or of one aspect of a depth-stencil format, go through
// command memory (needsStaging): Metal's own copy needs equal formats and
// moves both aspects. Metal has no layouts, so a discard is the
// validation bookkeeping plus the full barrier RI promises (P69).
//
// D3D12 counterpart: the copy, clear and discard sections of
// backends/d3d12/Rendering.cpp; Vulkan: backends/vulkan/Commands.cpp.

#include "DeviceState.hpp"

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

// D3D12's footprint rules (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT), the portable answer.
constexpr uint64 kRowPitchAlignment = 256;
constexpr uint64 kLayerAlignment = 512;

uint64 roundUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

MTL::Texture *resourceOf(const TextureView &view) { return static_cast<MTL::Texture *>(view.internal.resource); }

bool isVolume(const TextureView &view) { return view.internal.shape == TextureShape::e3D; }

uint32 copyBytesOf(const TextureView &view) {
    if (view.internal.planes == 3) {
        return 0; // one aspect at a time
    }
    return copyBytesPerTexel(view.format, view.internal.planes == 2 ? Aspect::eStencil : Aspect::eDefault);
}

// Which aspect of a depth-stencil format a buffer copy moves.
MTL::BlitOption blitOptionOf(const TextureView &view) {
    if (!hasStencil(view.format)) {
        return MTL::BlitOptionNone;
    }
    return view.internal.planes == 2 ? MTL::BlitOptionStencilFromDepthStencil : MTL::BlitOptionDepthFromDepthStencil;
}

void checkCopy([[maybe_unused]] const TextureView &view, [[maybe_unused]] bool bufferCopy,
               [[maybe_unused]] TextureUsage usage, [[maybe_unused]] std::string_view usageName,
               [[maybe_unused]] std::string_view call) {
    EQ_VALIDATE(Validation::checkCopyView(view.internal.mipCount, view.samples, bufferCopy, copyBytesOf(view),
                                          (view.internal.textureUsage & usage) != TextureUsage::eNone, usageName,
                                          call));
}

// A copy region (P108): the default is the whole mip.
Rect resolved(Rect region, const TextureView &view) {
    return region.width == 0 && region.height == 0 ? Rect{0, 0, view.width, view.height} : region;
}

// Depth, stencil and multisampled views copy only whole: D3D12's rule, kept
// on every backend.
[[maybe_unused]] bool copiesWhole(const TextureView &view) { return view.samples > 1 || isDepthFormat(view.format); }

void checkRegion([[maybe_unused]] Rect region, [[maybe_unused]] const TextureView &view,
                 [[maybe_unused]] std::string_view what, [[maybe_unused]] std::string_view call) {
    EQ_VALIDATE(Validation::checkCopyRegion(region.x, region.y, region.width, region.height, view.width, view.height,
                                            copiesWhole(view), what, call));
}

void checkBufferCopyRange([[maybe_unused]] const Buffer &buffer, [[maybe_unused]] uint64 offset,
                          [[maybe_unused]] const CopyLayout &layout, BufferUsage usage, std::string_view call) {
    EQ_VALIDATE(Validation::checkRange(offset, layout.size, buffer.size, layout.alignment,
                                       std::format("{}: the texels in the buffer", call)));
    if ((buffer.usage & usage) == BufferUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the buffer needs BufferUsage::{}", call,
                                usage == BufferUsage::eCopySrc ? "eCopySrc" : "eCopyDst"));
    }
}

// Layer `layer` of a one-mip view (counted from its first): a slice of a 2D
// array or cube, or a depth (z) of a volume.
uint32 sliceOf(const TextureView &view, uint32 layer) { return isVolume(view) ? 0 : view.internal.baseLayer + layer; }
uint32 zOf(const TextureView &view, uint32 layer) { return isVolume(view) ? view.internal.baseLayer + layer : 0; }

// Metal copies texture to texture only between equal pixel formats, and
// moves both aspects of a depth-stencil format. RI copies any formats of the
// same texel size, one aspect at a time (as D3D12's planes and Vulkan's
// aspect masks): those copies go through command memory instead.
bool needsStaging(const TextureView &src, const TextureView &dst) {
    return resourceOf(src)->pixelFormat() != resourceOf(dst)->pixelFormat() ||
           blitOptionOf(src) != MTL::BlitOptionNone || blitOptionOf(dst) != MTL::BlitOptionNone;
}

// Bytes are bytes in a buffer: `region` of each layer of `src` goes out in
// copyLayout's layout, then into `dst` at `dstOffset`, each with its aspect.
void copyThroughBuffer(DeviceState &state, CommandListState &cmd, const TextureView &src, const TextureView &dst,
                       Rect region, Offset dstOffset) {
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, src, region);
    const CommandMemoryAllocation staging = allocateCommandMemory(state, cmd, layout.size, layout.alignment);
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(cmd);
    const uint64 imageBytes = layout.rowPitch * layout.height;
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        encoder->copyFromTexture(resourceOf(src), sliceOf(src, layer), src.internal.baseMip,
                                 MTL::Origin(region.x, region.y, zOf(src, layer)),
                                 MTL::Size(region.width, region.height, 1), staging.buffer,
                                 staging.offset + layer * layout.layerPitch, layout.rowPitch, imageBytes,
                                 blitOptionOf(src));
    }
    // The second half reads what the first wrote.
    encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice);
    for (uint32 layer = 0; layer < layout.layers; ++layer) {
        encoder->copyFromBuffer(staging.buffer, staging.offset + layer * layout.layerPitch, layout.rowPitch,
                                imageBytes, MTL::Size(region.width, region.height, 1), resourceOf(dst),
                                sliceOf(dst, layer), dst.internal.baseMip,
                                MTL::Origin(dstOffset.x, dstOffset.y, zOf(dst, layer)), blitOptionOf(dst));
    }
}

// ── Clear values as texel bytes (storage textures, blit clears) ──────────────

uint8 unorm8(float value) { return static_cast<uint8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

float linearToSrgb(float value) {
    const float v = std::clamp(value, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

uint16 halfBits(float value) { return std::bit_cast<uint16>(static_cast<_Float16>(value)); }

// The bytes of one texel of `format` holding `color` (RGBA), as the GPU
// stores them; `out` holds at least 16 bytes. Returns the texel size.
uint32 encodeTexel(Format format, const std::array<float, 4> &color, std::byte *out) {
    const auto put = [&](const auto &value) {
        std::memcpy(out, &value, sizeof(value));
        return static_cast<uint32>(sizeof(value));
    };
    const auto uintOf = [](float v) { return static_cast<uint32>(std::max(v, 0.0f)); };
    switch (format) {
    case Format::eR8Unorm: return put(unorm8(color[0]));
    case Format::eRG8Unorm: return put(std::array<uint8, 2>{unorm8(color[0]), unorm8(color[1])});
    case Format::eRGBA8Unorm:
        return put(std::array<uint8, 4>{unorm8(color[0]), unorm8(color[1]), unorm8(color[2]), unorm8(color[3])});
    case Format::eRGBA8Srgb:
        return put(std::array<uint8, 4>{unorm8(linearToSrgb(color[0])), unorm8(linearToSrgb(color[1])),
                                        unorm8(linearToSrgb(color[2])), unorm8(color[3])});
    case Format::eBGRA8Unorm:
        return put(std::array<uint8, 4>{unorm8(color[2]), unorm8(color[1]), unorm8(color[0]), unorm8(color[3])});
    case Format::eBGRA8Srgb:
        return put(std::array<uint8, 4>{unorm8(linearToSrgb(color[2])), unorm8(linearToSrgb(color[1])),
                                        unorm8(linearToSrgb(color[0])), unorm8(color[3])});
    case Format::eRGB10A2Unorm: {
        const auto bits = [&](float v, float max) {
            return static_cast<uint32>(std::lround(std::clamp(v, 0.0f, 1.0f) * max));
        };
        return put(bits(color[0], 1023.0f) | bits(color[1], 1023.0f) << 10 | bits(color[2], 1023.0f) << 20 |
                   bits(color[3], 3.0f) << 30);
    }
    case Format::eR11G11B10Float: {
        // Unsigned small floats: a half's exponent and top mantissa bits.
        const auto small = [](float v, uint32 drop) { return uint32{halfBits(std::max(v, 0.0f))} >> drop; };
        return put((small(color[0], 4) & 0x7ff) | (small(color[1], 4) & 0x7ff) << 11 |
                   (small(color[2], 5) & 0x3ff) << 22);
    }
    case Format::eR16Float: return put(halfBits(color[0]));
    case Format::eRG16Float: return put(std::array<uint16, 2>{halfBits(color[0]), halfBits(color[1])});
    case Format::eRGBA16Float:
        return put(std::array<uint16, 4>{halfBits(color[0]), halfBits(color[1]), halfBits(color[2]), halfBits(color[3])});
    case Format::eR32Float: return put(color[0]);
    case Format::eRG32Float: return put(std::array<float, 2>{color[0], color[1]});
    case Format::eRGBA32Float: return put(color);
    case Format::eR32Uint: return put(uintOf(color[0]));
    case Format::eRG32Uint: return put(std::array<uint32, 2>{uintOf(color[0]), uintOf(color[1])});
    case Format::eRGBA32Uint:
        return put(std::array<uint32, 4>{uintOf(color[0]), uintOf(color[1]), uintOf(color[2]), uintOf(color[3])});
    default: return 0; // depth formats: cleared by render passes
    }
}

// Every (mip, layer) a clear covers, for `visit(mip, width, height, slice, z)`;
// mips count from the view's first, slices and z are absolute.
template <typename Visit>
void forEachSubresource(const TextureView &view, Visit visit) {
    for (uint32 mip = 0; mip < view.internal.mipCount; ++mip) {
        const uint32 width = std::max(1u, view.width >> mip);
        const uint32 height = std::max(1u, view.height >> mip);
        if (isVolume(view)) {
            // One mip: the view's slices; several: every slice of each mip (P102).
            const uint32 depth = std::max(1u, view.internal.textureLayers >> (view.internal.baseMip + mip));
            const uint32 first = view.internal.mipCount == 1 ? view.internal.baseLayer : 0;
            const uint32 count = view.internal.mipCount == 1 ? view.internal.layerCount : depth;
            for (uint32 z = first; z < first + count; ++z) {
                visit(mip, width, height, 0u, z);
            }
        } else {
            for (uint32 layer = 0; layer < view.internal.layerCount; ++layer) {
                visit(mip, width, height, view.internal.baseLayer + layer, 0u);
            }
        }
    }
}

// Storage textures: one row of the value from command memory, doubled down
// the first layer, then copied to the others. Blits only, so the clear is a
// copy for barriers with no encoder change.
void clearByBlit(DeviceState &state, CommandListState &cmd, const TextureView &view, const ClearValue &value) {
    std::byte texel[16];
    const uint32 bytes = encodeTexel(view.format, value.color, texel);
    MTL::Texture *texture = resourceOf(view);
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(cmd);
    // The first (slice, z) of the current mip: the source of its other layers.
    uint32 currentMip = ~0u, firstSlice = 0, firstZ = 0;
    forEachSubresource(view, [&](uint32 mip, uint32 width, uint32 height, uint32 slice, uint32 z) {
        const uint32 level = view.internal.baseMip + mip;
        if (mip == currentMip) {
            encoder->copyFromTexture(texture, firstSlice, level, MTL::Origin(0, 0, firstZ),
                                     MTL::Size(width, height, 1), texture, slice, level, MTL::Origin(0, 0, z));
            return;
        }
        currentMip = mip;
        firstSlice = slice;
        firstZ = z;
        const uint64 rowBytes = uint64{width} * bytes;
        const CommandMemoryAllocation row = allocateCommandMemory(state, cmd, rowBytes, 16);
        for (uint32 x = 0; x < width; ++x) {
            std::memcpy(row.cpu + uint64{x} * bytes, texel, bytes);
        }
        encoder->copyFromBuffer(row.buffer, row.offset, rowBytes, rowBytes, MTL::Size(width, 1, 1), texture, slice,
                                level, MTL::Origin(0, 0, z));
        for (uint32 done = 1; done < height;) {
            const uint32 step = std::min(done, height - done);
            // Each copy reads what the previous one wrote.
            encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice);
            encoder->copyFromTexture(texture, slice, level, MTL::Origin(0, 0, z), MTL::Size(width, step, 1), texture,
                                     slice, level, MTL::Origin(0, done, z));
            done += step;
        }
        // The other layers copy the finished first one.
        encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice);
    });
}

// Render targets and depth: a render pass per mip and layer whose load action
// clears; MTL4RenderPassDescriptor attachments name one slice or depth plane.
void clearByRenderPass(CommandListState &cmd, const TextureView &view, const ClearValue &value) {
    const bool depth = isDepthFormat(view.format);
    // Color through the view, so its format encodes the value (sRGB views);
    // depth and stencil through the texture, whose format has both aspects.
    auto *target = depth ? resourceOf(view) : static_cast<MTL::Texture *>(view.internal.texture);
    forEachSubresource(view, [&](uint32 mip, uint32, uint32, uint32 slice, uint32 z) {
        NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
        MTL4::RenderPassDescriptor *pass = MTL4::RenderPassDescriptor::alloc()->init();
        const auto attach = [&](MTL::RenderPassAttachmentDescriptor *attachment, bool clear) {
            attachment->setTexture(target);
            attachment->setLevel(depth ? view.internal.baseMip + mip : mip);
            if (isVolume(view)) {
                attachment->setDepthPlane(z);
            } else {
                attachment->setSlice(depth ? slice : slice - view.internal.baseLayer);
            }
            attachment->setLoadAction(clear ? MTL::LoadActionClear : MTL::LoadActionLoad);
            attachment->setStoreAction(MTL::StoreActionStore);
        };
        if (!depth) {
            MTL::RenderPassColorAttachmentDescriptor *color = pass->colorAttachments()->object(0);
            attach(color, true);
            color->setClearColor(MTL::ClearColor(value.color[0], value.color[1], value.color[2], value.color[3]));
        } else {
            // A view of one aspect clears that aspect; the other is kept.
            MTL::RenderPassDepthAttachmentDescriptor *depthAttachment = pass->depthAttachment();
            attach(depthAttachment, (view.internal.planes & 1u) != 0);
            depthAttachment->setClearDepth(value.depth);
            if (hasStencil(view.format)) {
                MTL::RenderPassStencilAttachmentDescriptor *stencil = pass->stencilAttachment();
                attach(stencil, (view.internal.planes & 2u) != 0);
                stencil->setClearStencil(value.stencil);
            }
        }
        runClearPass(cmd, pass);
        pass->release();
        pool->release();
    });
}

} // namespace

// ── Copies (P47, P108) ───────────────────────────────────────────────────────

CopyLayout copyLayout(Device &, const TextureView &view, Rect region) {
    checkRegion(region, view, "view", "copyLayout");
    region = resolved(region, view);
    CopyLayout layout;
    layout.width = region.width;
    layout.height = region.height;
    layout.layers = view.internal.layerCount;
    layout.bytesPerTexel = copyBytesOf(view);
    if (layout.bytesPerTexel == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    "copyLayout: the view has both depth and stencil; copies take one aspect (P47)");
    }
    layout.alignment = kLayerAlignment;
    layout.rowPitch = roundUp(uint64{layout.width} * layout.bytesPerTexel, kRowPitchAlignment);
    const uint64 layerBytes = layout.rowPitch * layout.height;
    // 3D slices follow each other directly, as D3D12 requires (P102).
    layout.layerPitch = isVolume(view) ? layerBytes : roundUp(layerBytes, kLayerAlignment);
    layout.size = layout.layerPitch * (layout.layers - 1) + layerBytes;
    return layout;
}

void cmdCopyBufferToTexture(CommandList &cmd, const Buffer &src, uint64 srcOffset, const TextureView &dst,
                            Rect region) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, src.internal.id, "buffer");
    EQ_MTL_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdCopyBufferToTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyBufferToTexture"));
#endif
    checkCopy(dst, true, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyBufferToTexture");
    checkRegion(region, dst, "destination", "cmdCopyBufferToTexture");
    region = resolved(region, dst);
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, dst, region);
    checkBufferCopyRange(src, srcOffset, layout, BufferUsage::eCopySrc, "cmdCopyBufferToTexture");
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);
    // A volume's slices in one copy (bytesPerImage is the slice pitch); a 2D
    // array's layers one copy each.
    const uint32 copies = isVolume(dst) ? 1 : layout.layers;
    for (uint32 layer = 0; layer < copies; ++layer) {
        encoder->copyFromBuffer(static_cast<MTL::Buffer *>(src.internal.buffer), srcOffset + layer * layout.layerPitch,
                                layout.rowPitch, layout.rowPitch * layout.height,
                                MTL::Size(region.width, region.height, isVolume(dst) ? layout.layers : 1),
                                resourceOf(dst), sliceOf(dst, layer), dst.internal.baseMip,
                                MTL::Origin(region.x, region.y, zOf(dst, layer)), blitOptionOf(dst));
    }
}

void cmdCopyTextureToBuffer(CommandList &cmd, const TextureView &src, const Buffer &dst, uint64 dstOffset,
                            Rect region) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_MTL_CHECK_ALIVE(&state, dst.internal.id, "buffer");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdCopyTextureToBuffer"));
    checkCopy(src, true, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTextureToBuffer");
    checkRegion(region, src, "source", "cmdCopyTextureToBuffer");
    region = resolved(region, src);
    Device device{.internal = {.state = &state}};
    const CopyLayout layout = copyLayout(device, src, region);
    checkBufferCopyRange(dst, dstOffset, layout, BufferUsage::eCopyDst, "cmdCopyTextureToBuffer");
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);
    const uint32 copies = isVolume(src) ? 1 : layout.layers;
    for (uint32 layer = 0; layer < copies; ++layer) {
        encoder->copyFromTexture(resourceOf(src), sliceOf(src, layer), src.internal.baseMip,
                                 MTL::Origin(region.x, region.y, zOf(src, layer)),
                                 MTL::Size(region.width, region.height, isVolume(src) ? layout.layers : 1),
                                 static_cast<MTL::Buffer *>(dst.internal.buffer), dstOffset + layer * layout.layerPitch,
                                 layout.rowPitch, layout.rowPitch * layout.height, blitOptionOf(src));
    }
}

void cmdCopyTexture(CommandList &cmd, const TextureView &src, const TextureView &dst, Rect region, Offset dstOffset) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_MTL_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdCopyTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdCopyTexture"));
#endif
    checkCopy(src, false, TextureUsage::eCopySrc, "eCopySrc", "cmdCopyTexture");
    checkCopy(dst, false, TextureUsage::eCopyDst, "eCopyDst", "cmdCopyTexture");
    EQ_VALIDATE(Validation::checkCopyTextureMatch(src.internal.layerCount, src.samples, copyBytesOf(src),
                                                  dst.internal.layerCount, dst.samples, copyBytesOf(dst)));
    checkRegion(region, src, "source", "cmdCopyTexture");
    region = resolved(region, src);
    checkRegion({dstOffset.x, dstOffset.y, region.width, region.height}, dst, "destination", "cmdCopyTexture");
    MTL4::ComputeCommandEncoder *encoder = computeEncoder(list);
    if (needsStaging(src, dst)) {
        copyThroughBuffer(state, list, src, dst, region, dstOffset);
        return;
    }
    // One slice or layer at a time, so 3D and 2D-array views copy into each other.
    for (uint32 layer = 0; layer < src.internal.layerCount; ++layer) {
        encoder->copyFromTexture(resourceOf(src), sliceOf(src, layer), src.internal.baseMip,
                                 MTL::Origin(region.x, region.y, zOf(src, layer)),
                                 MTL::Size(region.width, region.height, 1), resourceOf(dst), sliceOf(dst, layer),
                                 dst.internal.baseMip, MTL::Origin(dstOffset.x, dstOffset.y, zOf(dst, layer)));
    }
}

// ── Discard and clears (P65, P69) ────────────────────────────────────────────

// No layouts or metadata to reset on Metal: the bookkeeping and RI's full
// barrier, so later work sees the texture as new.
void cmdDiscard(CommandList &cmd, [[maybe_unused]] const TextureView &view) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, view.internal.textureId, "texture");
    EQ_VALIDATE(Validation::checkRenderingSection(cmd.internal.state->rendering, false, "cmdDiscard"));
#if EQUINE_VALIDATION
    Validation::markDiscarded(state.placements, view.internal.textureId);
#endif
    cmdBarrier(cmd, Stage::eAll, Stage::eAll);
}

void cmdClearTexture(CommandList &cmd, const TextureView &view, const ClearValue &value) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    DeviceState &state = *cmd.internal.device;
    EQ_MTL_CHECK_ALIVE(&state, view.internal.textureId, "texture");
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, false, "cmdClearTexture"));
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, view.internal.textureId, "cmdClearTexture"));
#endif
    const TextureUsage usage = view.internal.textureUsage;
    if ((usage & (TextureUsage::eRenderTarget | TextureUsage::eDepthStencil)) != TextureUsage::eNone) {
        clearByRenderPass(list, view, value);
        return;
    }
    // D3D12's rule and message: clears need an attachment or storage usage.
    if ((usage & TextureUsage::eStorage) == TextureUsage::eNone) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdClearTexture: the texture needs eRenderTarget, eDepthStencil or eStorage usage");
    }
    clearByBlit(state, list, view, value);
}

} // namespace RI::Metal
