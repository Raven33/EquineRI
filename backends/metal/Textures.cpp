// Textures and views on Metal (P58, P69, P73, P74, P102, P114 (3)).
//
// Owns: the format table (pixelFormatOf, formatName), formatSupported from
// Metal's pixel format capabilities on Apple7+, textureRequirements, textures
// placed in a Memory's heap, and views (Metal texture views).
//
// Watch out: MSC 4 converts HLSL texture types to the matching Metal types
// (non-array by default), so a view's MTLTextureType must be the type the
// shader declares, as D3D12 picks SRV and UAV dimensions: an array when the
// texture has layers > 1, a cube for whole cubes, 3D for volumes, multisampled
// when samples > 1. A whole-cube view also keeps a 2D-array view
// (arrayTexture) for storage writes, which see the faces as a 2D array (as
// Vulkan's arrayView). Views join the residency set although their heap is
// resident: Metal's shader validation blocks, silently (reads return 0),
// textures it finds in no residency set. Metal has no layouts (P16): a new
// texture needs no reset submission, and placed textures share their heap's
// storage mode, so they live in eGPU memory only, as on D3D12. Metal reports
// no pixel format capabilities at run time: formatSupported is a table of
// Apple's feature set tables for Apple7+, plus the runtime queries Metal has
// (D24S8, sample counts).
//
// Vulkan counterpart: backends/vulkan/Textures.cpp; D3D12: backends/d3d12/Textures.cpp.

#include "DeviceState.hpp"

#include <RI/Metal/Native.hpp>
#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <format>

namespace RI::Metal {

namespace {

// What Apple7+ GPUs do with a format (Metal feature set tables, "Apple7").
struct FormatInfo {
    Format format;
    MTL::PixelFormat native;
    const char *name;
    bool storage; // shader writes (MTLTextureUsageShaderWrite)
    bool color;   // color render target
    bool msaa;
};

// Every RI format, in enum order. sRGB formats have no shader writes on Apple
// GPUs; depth formats are never written by shaders.
constexpr FormatInfo kFormats[] = {
    {Format::eUndefined, MTL::PixelFormatInvalid, "eUndefined", false, false, false},
    {Format::eR8Unorm, MTL::PixelFormatR8Unorm, "eR8Unorm", true, true, true},
    {Format::eRG8Unorm, MTL::PixelFormatRG8Unorm, "eRG8Unorm", true, true, true},
    {Format::eRGBA8Unorm, MTL::PixelFormatRGBA8Unorm, "eRGBA8Unorm", true, true, true},
    {Format::eRGBA8Srgb, MTL::PixelFormatRGBA8Unorm_sRGB, "eRGBA8Srgb", false, true, true},
    {Format::eBGRA8Unorm, MTL::PixelFormatBGRA8Unorm, "eBGRA8Unorm", true, true, true},
    {Format::eBGRA8Srgb, MTL::PixelFormatBGRA8Unorm_sRGB, "eBGRA8Srgb", false, true, true},
    {Format::eRGB10A2Unorm, MTL::PixelFormatRGB10A2Unorm, "eRGB10A2Unorm", true, true, true},
    {Format::eR11G11B10Float, MTL::PixelFormatRG11B10Float, "eR11G11B10Float", true, true, true},
    {Format::eR16Float, MTL::PixelFormatR16Float, "eR16Float", true, true, true},
    {Format::eRG16Float, MTL::PixelFormatRG16Float, "eRG16Float", true, true, true},
    {Format::eRGBA16Float, MTL::PixelFormatRGBA16Float, "eRGBA16Float", true, true, true},
    {Format::eR32Float, MTL::PixelFormatR32Float, "eR32Float", true, true, true},
    {Format::eRG32Float, MTL::PixelFormatRG32Float, "eRG32Float", true, true, true},
    {Format::eRGBA32Float, MTL::PixelFormatRGBA32Float, "eRGBA32Float", true, true, true},
    {Format::eR32Uint, MTL::PixelFormatR32Uint, "eR32Uint", true, true, true},
    {Format::eRG32Uint, MTL::PixelFormatRG32Uint, "eRG32Uint", true, true, true},
    {Format::eRGBA32Uint, MTL::PixelFormatRGBA32Uint, "eRGBA32Uint", true, true, true},
    {Format::eD16Unorm, MTL::PixelFormatDepth16Unorm, "eD16Unorm", false, false, true},
    {Format::eD32Float, MTL::PixelFormatDepth32Float, "eD32Float", false, false, true},
    // Not on Apple silicon (isDepth24Stencil8PixelFormatSupported says so).
    {Format::eD24UnormS8Uint, MTL::PixelFormatDepth24Unorm_Stencil8, "eD24UnormS8Uint", false, false, true},
    {Format::eD32FloatS8Uint, MTL::PixelFormatDepth32Float_Stencil8, "eD32FloatS8Uint", false, false, true},
};

const FormatInfo &infoOf(Format format) {
    const auto index = static_cast<size_t>(format);
    if (index >= std::size(kFormats)) {
        reportError(ErrorCategory::eProgrammerError, std::format("unknown texture format {}", index));
    }
    return kFormats[index];
}

static_assert(std::size(kFormats) <= 64, "Texture::internal.viewFormats is a 64-bit mask");
uint64 formatBit(Format format) { return uint64{1} << static_cast<uint32>(format); }

bool has(TextureUsage usage, TextureUsage bit) { return (usage & bit) != TextureUsage::eNone; }

// The formats this GPU has at all: everything but D24S8 on Apple silicon.
bool formatExists(MTL::Device *device, Format format) {
    if (format == Format::eUndefined || static_cast<size_t>(format) >= std::size(kFormats)) {
        return false;
    }
    return format != Format::eD24UnormS8Uint || device->isDepth24Stencil8PixelFormatSupported();
}

void requireFormat(const Device &device, Format format, const char *call) {
    if (!formatExists(device.internal.state->device, format)) {
        reportError(ErrorCategory::eUnsupported,
                    std::format("{}: format {} is not available on '{}' (check formatSupported, P58 (2))", call,
                                formatName(format), device.gpu.name));
    }
}

// Native usage. Storage reads back through RWTexture loads; render targets
// and depth are also cleared through render passes (P65). A texture with
// only copy usages still gets ShaderRead: Metal reads Unknown as "every
// usage", which costs more.
MTL::TextureUsage usageOf(const TextureDesc &desc) {
    NS::UInteger flags = 0;
    if (has(desc.usage, TextureUsage::eSampled | TextureUsage::eStorage) ||
        !has(desc.usage, TextureUsage::eRenderTarget | TextureUsage::eDepthStencil)) {
        flags |= MTL::TextureUsageShaderRead;
    }
    if (has(desc.usage, TextureUsage::eStorage)) {
        flags |= MTL::TextureUsageShaderWrite;
    }
    if (has(desc.usage, TextureUsage::eRenderTarget | TextureUsage::eDepthStencil)) {
        flags |= MTL::TextureUsageRenderTarget;
    }
    // Views in other formats (P74), and stencil views of a depth-stencil texture.
    if (!desc.viewFormats.empty() || (hasStencil(desc.format) && has(desc.usage, TextureUsage::eSampled))) {
        flags |= MTL::TextureUsagePixelFormatView;
    }
    return flags;
}

MTL::TextureType textureTypeOf(const TextureDesc &desc) {
    switch (desc.shape) {
    case TextureShape::e3D: return MTL::TextureType3D;
    case TextureShape::eCube: return desc.layers > 6 ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube;
    case TextureShape::e2D: break;
    }
    if (desc.samples > 1) {
        return desc.layers > 1 ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample;
    }
    return desc.layers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
}

// The descriptor of `desc` in private storage (eGPU memory), untracked (RI
// synchronizes, P36). The caller releases it.
MTL::TextureDescriptor *newDescriptor(const TextureDesc &desc) {
    MTL::TextureDescriptor *native = MTL::TextureDescriptor::alloc()->init();
    const MTL::TextureType type = textureTypeOf(desc);
    native->setTextureType(type);
    native->setPixelFormat(pixelFormatOf(desc.format));
    native->setWidth(desc.width);
    native->setHeight(desc.height);
    native->setDepth(desc.shape == TextureShape::e3D ? desc.depth : 1);
    native->setMipmapLevelCount(desc.mips);
    native->setSampleCount(desc.samples);
    // Metal counts cubes, not faces, in a cube array.
    native->setArrayLength(desc.shape == TextureShape::eCube ? std::max(desc.layers / 6, 1u)
                                                             : (desc.shape == TextureShape::e3D ? 1 : desc.layers));
    native->setUsage(usageOf(desc));
    native->setResourceOptions(MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked);
    return native;
}

uint8 planesOf(Format format, Aspect aspect) {
    if (!isDepthFormat(format)) {
        return aspect == Aspect::eDefault || aspect == Aspect::eColor ? 1 : 0;
    }
    switch (aspect) {
    case Aspect::eDefault: return hasStencil(format) ? 3 : 1;
    case Aspect::eDepth: return 1;
    case Aspect::eStencil: return hasStencil(format) ? 2 : 0;
    case Aspect::eColor: return 0;
    }
    return 0;
}

// The pixel format a view reads: the stencil aspect of a depth-stencil
// texture is Metal's X*_Stencil8 view; a depth view of one reads depth.
MTL::PixelFormat viewPixelFormatOf(Format format, uint8 planes) {
    if (planes == 2) {
        return format == Format::eD24UnormS8Uint ? MTL::PixelFormatX24_Stencil8 : MTL::PixelFormatX32_Stencil8;
    }
    return pixelFormatOf(format);
}

void label(MTL::Texture *texture, std::string_view name) {
    if (texture != nullptr) {
        NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
        texture->setLabel(nsString(name));
        pool->release();
    }
}

TextureView makeView(DeviceState &state, const Texture &texture, const TextureViewDesc &desc) {
    const Format format = desc.format == Format::eUndefined ? texture.format : desc.format;
    // In every build: Metal faults on a view format the texture was not made for (P74).
    if (format != texture.format && (texture.internal.viewFormats & formatBit(format)) == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createView: format {} is not the texture's format or one of its viewFormats (P74)",
                                formatName(format)));
    }
    EQ_VALIDATE(Validation::checkViewFormatCompatible(texture.format, format));
    const uint8 planes = planesOf(texture.format, desc.aspect);
    if (planes == 0) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createView: the texture's format {} has no such aspect", formatName(texture.format)));
    }
    const uint32 mipCount =
        desc.mipCount == kAllMips ? texture.mips - std::min(desc.baseMip, texture.mips) : desc.mipCount;
    // A 3D texture's layers are the depth slices of the view's first mip.
    const bool volume = texture.shape == TextureShape::e3D;
    const uint32 layers = volume ? std::max(1u, texture.depth >> std::min(desc.baseMip, 31u)) : texture.layers;
    const uint32 layerCount =
        desc.layerCount == kAllLayers ? layers - std::min(desc.baseLayer, layers) : desc.layerCount;
    EQ_VALIDATE(Validation::checkViewRange(desc.baseMip, mipCount, texture.mips, desc.baseLayer, layerCount, layers));
    if (volume) {
        EQ_VALIDATE(Validation::checkVolumeView(mipCount, desc.baseLayer != 0 || layerCount != layers));
    }
    const bool wholeCubes =
        texture.shape == TextureShape::eCube && desc.baseLayer % 6 == 0 && layerCount % 6 == 0 && layerCount > 0;

    // The type the shader declares (MSC 4, non-array by default): arrays follow
    // the texture, as on D3D12 and in HLSL; whole cubes read as cubes.
    MTL::TextureType type = texture.layers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
    if (texture.samples > 1) {
        type = texture.layers > 1 ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample;
    } else if (wholeCubes) {
        type = layerCount == 6 ? MTL::TextureTypeCube : MTL::TextureTypeCubeArray;
    } else if (volume) {
        type = MTL::TextureType3D;
    }
    auto *resource = static_cast<MTL::Texture *>(texture.internal.texture);
    const MTL::PixelFormat pixelFormat = viewPixelFormatOf(format, planes);
    // A volume view covers every slice: the slice range only picks render targets.
    const NS::Range levels(desc.baseMip, mipCount);
    const NS::Range slices = volume ? NS::Range(0, 1) : NS::Range(desc.baseLayer, layerCount);
    MTL::Texture *view = resource->newTextureView(pixelFormat, type, levels, slices);
    if (view == nullptr) {
        reportError(ErrorCategory::eOutOfMemory, "createView: newTextureView failed");
    }
    MTL::Texture *arrayView = nullptr;
    if (wholeCubes && has(texture.usage, TextureUsage::eStorage)) {
        arrayView = resource->newTextureView(pixelFormat, MTL::TextureType2DArray, levels, slices);
        if (arrayView == nullptr) {
            view->release();
            reportError(ErrorCategory::eOutOfMemory, "createView: newTextureView failed (2D-array view of cubes)");
        }
    }

    // Heap residency covers the texture, but Metal's shader validation looks
    // views up by resource ID and blocks those outside a residency set: views
    // join the set too (no cost to the GPU, the heap is resident anyway).
    makeResident(state, view);
    if (arrayView != nullptr) {
        makeResident(state, arrayView);
    }

    TextureView out;
    out.format = format;
    out.width = std::max(1u, texture.width >> desc.baseMip);
    out.height = std::max(1u, texture.height >> desc.baseMip);
    out.samples = texture.samples;
    auto &in = out.internal;
    in.texture = view;
    in.arrayTexture = arrayView;
    in.resource = resource;
    in.baseMip = desc.baseMip;
    in.mipCount = mipCount;
    in.baseLayer = desc.baseLayer;
    in.layerCount = layerCount;
    in.shape = texture.shape;
    in.cube = wholeCubes;
    in.planes = planes;
    in.textureMips = texture.mips;
    in.textureLayers = volume ? texture.depth : texture.layers;
    in.textureUsage = texture.usage;
    in.textureId = texture.internal.id;
    if (!desc.name.empty()) {
        applyName(state, out, desc.name);
    }
    return out;
}

void releaseView(DeviceState &state, TextureView &view) {
    for (void *pointer : {view.internal.texture, view.internal.arrayTexture}) {
        auto *texture = static_cast<MTL::Texture *>(pointer);
        if (texture != nullptr) {
            makeNonResident(state, texture);
            release(texture);
        }
    }
}

} // namespace

// ── Formats ──────────────────────────────────────────────────────────────────

MTL::PixelFormat pixelFormatOf(Format format) { return infoOf(format).native; }
const char *formatName(Format format) { return infoOf(format).name; }

// Any thread (P106): reads the device's fixed capabilities only.
bool formatSupported(Device &device, Format format, TextureUsage usage, uint32 samples) {
    MTL::Device *native = device.internal.state->device;
    if (!formatExists(native, format)) {
        return false;
    }
    const FormatInfo &info = infoOf(format);
    const bool depth = isDepthFormat(format);
    if ((has(usage, TextureUsage::eStorage) && !info.storage) || (has(usage, TextureUsage::eRenderTarget) && !info.color) ||
        (has(usage, TextureUsage::eDepthStencil) && !depth)) {
        return false;
    }
    if (samples == 1) {
        return true;
    }
    // Metal has no shader writes to multisampled textures.
    return info.msaa && !has(usage, TextureUsage::eStorage) && native->supportsTextureSampleCount(samples);
}

// ── Textures ─────────────────────────────────────────────────────────────────

// Textures live in eGPU memory (private storage), as on D3D12.
MemoryRequirements textureRequirements(Device &device, const TextureDesc &desc) {
    requireFormat(device, desc.format, "textureRequirements");
    MTL::TextureDescriptor *native = newDescriptor(desc);
    const MTL::SizeAndAlign sizeAndAlign = device.internal.state->device->heapTextureSizeAndAlign(native);
    native->release();
    const uint64 alignment = std::max<uint64>(sizeAndAlign.align, 1);
    return {.size = (sizeAndAlign.size + alignment - 1) / alignment * alignment,
            .alignment = alignment,
            .kinds = 1u << static_cast<uint32>(MemoryKind::eGPU)};
}

Texture createTexture(Device &device, const Memory &memory, uint64 offset, const TextureDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, memory.internal.id, "memory");
    EQ_VALIDATE(Validation::checkTextureUsage(desc.format, desc.usage));
    EQ_VALIDATE(Validation::checkTextureShape(desc));
    const MemoryRequirements requirements = textureRequirements(device, desc);
    if (!fits(requirements, memory.kind)) {
        reportError(ErrorCategory::eUnsupported, "createTexture: textures need eGPU memory on Metal");
    }
    // Heaps are 64 KiB multiples (Resources.cpp): the rounded size is placeable.
    EQ_VALIDATE(Validation::checkRange(offset, requirements.size, (memory.size + 65535) / 65536 * 65536,
                                       requirements.alignment, "createTexture: placing the texture in memory"));

    MTL::TextureDescriptor *native = newDescriptor(desc);
    MTL::Texture *placed = static_cast<MTL::Heap *>(memory.internal.heap)->newTexture(native, offset);
    native->release();
    if (placed == nullptr) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("createTexture: Metal could not place a {} texture of {} bytes at offset {} in memory "
                                "of {} bytes",
                                formatName(desc.format), requirements.size, offset, memory.size));
    }

    Texture texture;
    texture.shape = desc.shape;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth = desc.depth;
    texture.layers = desc.layers;
    texture.mips = desc.mips;
    texture.samples = desc.samples;
    texture.format = desc.format;
    texture.usage = desc.usage;
    texture.internal.texture = placed;
    for (Format format : desc.viewFormats) {
        texture.internal.viewFormats |= formatBit(format);
    }
    texture.internal.id = EQ_MTL_TRACK(&state);
#if EQUINE_VALIDATION
    Validation::placeResource(state.placements, memory.internal.id, offset, requirements.size, texture.internal.id,
                              true);
#endif
    // No reset to submit (R16, P69): Metal has no layouts, and a new texture's
    // contents are undefined until written, as RI promises.
    texture.view = makeView(state, texture, {});
    if (!desc.name.empty()) {
        applyName(state, texture, desc.name);
    }
    return texture;
}

void destroyTexture(Device &device, Texture &texture) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, texture.internal.id, "texture");
    EQ_MTL_UNTRACK(&state, texture.internal.id);
#if EQUINE_VALIDATION
    Validation::markDiscarded(state.placements, texture.internal.id);
#endif
    releaseView(state, texture.view);
    if (!texture.internal.imported) { // P77: an imported texture stays the user's
        auto *native = static_cast<MTL::Texture *>(texture.internal.texture);
        release(native);
    }
    texture = {};
}

// ── Views (P73, P74) ─────────────────────────────────────────────────────────

TextureView createView(Device &device, const Texture &texture, const TextureViewDesc &desc) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    DeviceState &state = *device.internal.state;
    EQ_MTL_CHECK_ALIVE(&state, texture.internal.id, "texture");
    return makeView(state, texture, desc);
}

void destroyView(Device &device, TextureView &view) {
    EQ_MTL_CHECK_THREAD(device.internal.state);
    EQ_MTL_CHECK_ALIVE(device.internal.state, view.internal.textureId, "texture");
    releaseView(*device.internal.state, view);
    view = {};
}

// ── Names (P53, P106) ────────────────────────────────────────────────────────

void applyName([[maybe_unused]] DeviceState &state, const Texture &texture, std::string_view name) {
    label(static_cast<MTL::Texture *>(texture.internal.texture), name);
    label(static_cast<MTL::Texture *>(texture.view.internal.texture), name);
    label(static_cast<MTL::Texture *>(texture.view.internal.arrayTexture), name);
    EQ_MTL_TRACK_NAME(&state, texture.internal.id, name);
}

// Views have no tracked id: the name reaches captures only, as on D3D12 and Vulkan.
void applyName(DeviceState &, const TextureView &view, std::string_view name) {
    label(static_cast<MTL::Texture *>(view.internal.texture), name);
    label(static_cast<MTL::Texture *>(view.internal.arrayTexture), name);
}

} // namespace RI::Metal
