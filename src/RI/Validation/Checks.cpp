// Implements Checks.hpp: RI's own validation rules (P88, P91), one function per
// rule, each returning the message to report or nothing.

#include <RI/Validation/Checks.hpp>

#include <RI/Commands.hpp>
#include <RI/Resources.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>

namespace RI::Validation {

CheckResult checkSlot(uint32 slot, uint32 count, std::string_view heap) {
    if (slot >= count) {
        return std::format("slot {} is outside the {} heap (it has {} slots)", slot, heap, count);
    }
    return {};
}

CheckResult checkRootDataSize(uint64 given, uint32 declared) {
    if (declared == 0) {
        return std::format("root data of {} bytes set, but the pipeline's shader declares none "
                           "(mark it with EQ_ROOT_DATA)",
                           given);
    }
    if (given != declared) {
        return std::format("root data is {} bytes, but the pipeline's shader declares {} bytes", given, declared);
    }
    return {};
}

CheckResult checkCommandListIdle(uint64 lastSubmitted, uint64 completed, std::string_view action) {
    if (lastSubmitted > completed) {
        return std::format("{} a command list the GPU may still be executing: wait for its timeline point "
                           "(value {}) first; the queue has reached {}",
                           action, lastSubmitted, completed);
    }
    return {};
}

CheckResult checkRange(uint64 offset, uint64 size, uint64 total, uint64 alignment, std::string_view what) {
    if (offset > total || size > total - offset) {
        return std::format("{}: range [{}, {}) exceeds the {} bytes available", what, offset, offset + size, total);
    }
    if (alignment != 0 && offset % alignment != 0) {
        return std::format("{}: offset {} is not a multiple of the required alignment {}", what, offset, alignment);
    }
    return {};
}

CheckResult checkHeapSetBeforePipeline(bool heapSet) {
    if (!heapSet) {
        return std::string("cmdSetPipeline: set a descriptor heap with cmdSetHeap first "
                           "(D3D12 needs the heaps bound before a root signature that indexes them)");
    }
    return {};
}

CheckResult checkBufferView(uint64 offset, uint64 size, uint64 bufferSize, uint32 stride, bool constant) {
    if (constant) {
        if (CheckResult r = checkRange(offset, constantViewSize(size), bufferSize, 256,
                                       "constant buffer view (size rounded up to 256 bytes)")) {
            return r;
        }
        return {};
    }
    if (CheckResult r = checkRange(offset, size, bufferSize, 16, "buffer view")) {
        return r;
    }
    const uint64 element = stride == 0 ? 4 : stride;
    if (offset % element != 0 || size % element != 0 || size == 0) {
        return std::format("buffer view: offset {} and size {} must be non-empty multiples of the element size {}",
                           offset, size, element);
    }
    return {};
}

CheckResult checkRenderingSection(bool rendering, bool inside, std::string_view call) {
    if (inside && !rendering) {
        return std::format("{}: no rendering section is open (cmdBeginRendering first)", call);
    }
    if (!inside && rendering) {
        return std::format("{}: not allowed inside a rendering section (cmdEndRendering first)", call);
    }
    return {};
}

CheckResult checkViewFormatCompatible(Format texture, Format view) {
    if (texture != view && (bytesPerTexel(texture) == 0 || bytesPerTexel(texture) != bytesPerTexel(view))) {
        return std::format("createView: a view format must have the texture format's size per texel ({} bytes, "
                           "the view's has {}) (P74)",
                           bytesPerTexel(texture), bytesPerTexel(view));
    }
    return {};
}

CheckResult checkViewRange(uint32 baseMip, uint32 mipCount, uint32 mips, uint32 baseLayer, uint32 layerCount,
                           uint32 layers) {
    if (mipCount == 0 || baseMip >= mips || mipCount > mips - baseMip) {
        return std::format("createView: mips {}..{} are outside the texture's {} mips", baseMip,
                           uint64{baseMip} + mipCount, mips);
    }
    if (layerCount == 0 || baseLayer >= layers || layerCount > layers - baseLayer) {
        return std::format("createView: layers {}..{} are outside the texture's {} layers", baseLayer,
                           uint64{baseLayer} + layerCount, layers);
    }
    return {};
}

CheckResult checkTextureUsage(Format format, TextureUsage usage) {
    const auto has = [&](TextureUsage bit) { return (usage & bit) != TextureUsage::eNone; };
    if (has(TextureUsage::eReadOnlyDepthSampling) &&
        (!has(TextureUsage::eSampled) || !has(TextureUsage::eDepthStencil) || !isDepthFormat(format))) {
        return std::string("createTexture: eReadOnlyDepthSampling needs a depth format with eSampled and "
                           "eDepthStencil (P97)");
    }
    return {};
}

CheckResult checkTextureShape(const TextureDesc &desc) {
    if (desc.shape != TextureShape::e3D && desc.depth != 1) {
        return std::format("createTexture: depth is {}; only 3D textures (TextureShape::e3D) have a depth",
                           desc.depth);
    }
    if (desc.shape == TextureShape::eCube) {
        if (desc.width != desc.height) {
            return std::format("createTexture: a cube texture is square; this one is {}x{}", desc.width,
                               desc.height);
        }
        if (desc.layers == 0 || desc.layers % 6 != 0) {
            return std::format("createTexture: a cube texture has 6 layers per cube; this one has {}", desc.layers);
        }
    }
    if (desc.shape == TextureShape::e3D) {
        if (desc.layers != 1) {
            return std::format("createTexture: a 3D texture has depth slices, not layers; layers is {}",
                               desc.layers);
        }
        if (isDepthFormat(desc.format)) {
            return std::string("createTexture: a 3D texture cannot have a depth format");
        }
    }
    if (desc.shape != TextureShape::e2D && desc.samples != 1) {
        return std::string("createTexture: cube and 3D textures are not multisampled");
    }
    return {};
}

CheckResult checkViewport(const Viewport &viewport) {
    if (!(viewport.width > 0.0f) || !(viewport.height > 0.0f)) {
        return std::format("cmdSetViewport: the viewport is {}x{}; give a width and height above 0", viewport.width,
                           viewport.height);
    }
    const auto inRange = [](float depth) { return depth >= 0.0f && depth <= 1.0f; };
    if (!inRange(viewport.minDepth) || !inRange(viewport.maxDepth)) {
        return std::format("cmdSetViewport: the depth range {}..{} is outside 0..1", viewport.minDepth,
                           viewport.maxDepth);
    }
    return {};
}

CheckResult checkScissor(const Rect &scissor) {
    if (scissor.x < 0 || scissor.y < 0) {
        return std::format("cmdSetScissor: the scissor starts at ({}, {}); x and y must be 0 or more", scissor.x,
                           scissor.y);
    }
    return {};
}

CheckResult checkRenderingAttachments(size_t colorCount, bool hasDepth) {
    if (colorCount == 0 && !hasDepth) {
        return std::string("cmdBeginRendering: the RenderingDesc has no attachment; for a section without "
                           "attachments, call cmdBeginRendering(cmd, RenderingSize{width, height})");
    }
    return {};
}

CheckResult checkRenderingSize(const RenderingSize &size) {
    constexpr uint32 kMax = 16384;
    if (size.width == 0 || size.height == 0 || size.width > kMax || size.height > kMax) {
        return std::format("cmdBeginRendering: the size is {}x{}; width and height must be 1..{}", size.width,
                           size.height, kMax);
    }
    return {};
}

CheckResult checkQueryNesting(QueryType beginning, uint32 openStatistics) {
    if (beginning == QueryType::eMeshPrimitives && openStatistics > 0) {
        return std::string("cmdBeginQuery: a mesh-primitives query begun inside an open statistics query counts "
                           "nothing on some GPUs; begin it before the statistics query, or measure it alone");
    }
    return {};
}

CheckResult checkVolumeView(uint32 mipCount, bool someSlices) {
    if (someSlices && mipCount != 1) {
        return std::format("createView: a view of some depth slices of a 3D texture covers one mip; this one "
                           "covers {}",
                           mipCount);
    }
    return {};
}

CheckResult checkMarkerEnd(uint32 depth) {
    if (depth == 0) {
        return std::string("cmdEndMarker: no marker region is open (P53)");
    }
    return {};
}

CheckResult checkMarkersClosed(uint32 depth) {
    if (depth != 0) {
        return std::format("cmdEnd: {} marker region(s) left open; close each with cmdEndMarker (P53)", depth);
    }
    return {};
}

CheckResult checkCopyView(uint32 mipCount, uint32 samples, bool bufferCopy, uint32 bytesPerTexel, bool hasUsage,
                          std::string_view usage, std::string_view call) {
    if (mipCount != 1) {
        return std::format("{}: the view covers {} mips; copies take a view of one mip (P47)", call, mipCount);
    }
    if (bufferCopy && samples != 1) {
        return std::format("{}: a multisampled texture cannot be copied to or from a buffer", call);
    }
    if (bytesPerTexel == 0) {
        return std::format("{}: the view has both depth and stencil; copy one aspect "
                           "(a view with Aspect::eDepth or Aspect::eStencil)", call);
    }
    if (!hasUsage) {
        return std::format("{}: the texture needs TextureUsage::{}", call, usage);
    }
    return {};
}

CheckResult checkCopyTextureMatch(uint32 srcWidth, uint32 srcHeight, uint32 srcLayers, uint32 srcSamples,
                                  uint32 srcBytes, uint32 dstWidth, uint32 dstHeight, uint32 dstLayers,
                                  uint32 dstSamples, uint32 dstBytes) {
    if (srcWidth != dstWidth || srcHeight != dstHeight || srcLayers != dstLayers) {
        return std::format("cmdCopyTexture: the source is {}x{} with {} layer(s), the destination {}x{} with {}; "
                           "they must match",
                           srcWidth, srcHeight, srcLayers, dstWidth, dstHeight, dstLayers);
    }
    if (srcSamples != dstSamples) {
        return std::string("cmdCopyTexture: the source and destination sample counts differ");
    }
    if (srcBytes != dstBytes) {
        return std::format("cmdCopyTexture: texels of {} and {} bytes cannot be copied into each other", srcBytes,
                           dstBytes);
    }
    return {};
}

CheckResult checkQueryKind(bool ok, std::string_view call) {
    if (!ok) {
        return std::format("{}: wrong query pool type (timestamps use cmdWriteTimestamp; occlusion and "
                           "pipeline statistics use cmdBeginQuery/cmdEndQuery)", call);
    }
    return {};
}

CheckResult checkGraphicsState(size_t colorCount, size_t blendCount, bool stencilTest, bool depthHasStencil) {
    if (blendCount != 0 && blendCount != colorCount) {
        return std::format("createGraphicsPipeline: {} blend states for {} color formats; give one per color "
                           "format, or none",
                           blendCount, colorCount);
    }
    if (stencilTest && !depthHasStencil) {
        return std::string("createGraphicsPipeline: the stencil test needs a depth format with stencil");
    }
    return {};
}

CheckResult checkGraphicsStages(bool mesh, bool hull, bool domain, bool geometry, bool patchList,
                                uint32 patchControlPoints) {
    if (mesh && (hull || domain || geometry)) {
        return std::string("createGraphicsPipeline: mesh pipelines take no hull, domain or geometry shader");
    }
    if (hull != domain) {
        return std::string("createGraphicsPipeline: give the hull and domain shaders together (tessellation)");
    }
    if (hull && !patchList) {
        return std::string("createGraphicsPipeline: hull and domain shaders need Topology::ePatchList");
    }
    if (!hull && patchList) {
        return std::string("createGraphicsPipeline: Topology::ePatchList needs hull and domain shaders");
    }
    if (patchList && (patchControlPoints < 1 || patchControlPoints > 32)) {
        return std::format("createGraphicsPipeline: patchControlPoints is {}; give 1 to 32 with "
                           "Topology::ePatchList",
                           patchControlPoints);
    }
    return {};
}

std::string stageSemantic(std::string_view semantic) {
    std::string result;
    for (const char c : semantic) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.back())) == 0) {
        result.push_back('0');
    }
    return result;
}

namespace {

std::string slotText(std::string_view slotName, const StageValue &value) {
    return value.component == 0 ? std::format("{} {}", slotName, value.slot)
                                : std::format("{} {}, component {}", slotName, value.slot, value.component);
}

} // namespace

CheckResult checkStageLink(std::span<const StageValue> outputs, std::span<const StageValue> inputs,
                           std::string_view producer, std::string_view slotName) {
    for (const StageValue &input : inputs) {
        const auto output = std::ranges::find(outputs, input.semantic, &StageValue::semantic);
        if (output == outputs.end()) {
            return std::format("createGraphicsPipeline: the pixel shader reads {}, which the {} shader does not "
                               "write; pass values from the {} to the pixel shader in one struct (P101)",
                               input.semantic, producer, producer);
        }
        if (output->slotChecked && (output->slot != input.slot || output->component != input.component)) {
            return std::format("createGraphicsPipeline: the pixel shader reads {} at {}, but the {} shader writes "
                               "it at {}: the pixel shader must take the {} shader's values in the same order, "
                               "leaving none out before the last one it reads; pass them in one struct (P101)",
                               input.semantic, slotText(slotName, input), producer, slotText(slotName, *output),
                               producer);
        }
    }
    return {};
}

CheckResult checkIndexBuffer(bool hasUsage, uint64 offset, uint32 indexSize, uint64 bufferSize) {
    if (!hasUsage) {
        return std::string("cmdSetIndexBuffer: the buffer needs BufferUsage::eIndex");
    }
    if (offset % indexSize != 0 || offset >= bufferSize) {
        return std::format("cmdSetIndexBuffer: offset {} must be a multiple of the index size ({} bytes) and inside "
                           "the buffer ({} bytes)",
                           offset, indexSize, bufferSize);
    }
    return {};
}

CheckResult checkResolve(const ResolveCheck &r, std::string_view call) {
    if (r.srcSamples < 2 || r.dstSamples != 1) {
        return std::format("{}: a resolve goes from a multisampled view into a single-sampled one ({} and {} samples)",
                           call, r.srcSamples, r.dstSamples);
    }
    if (r.srcWidth != r.dstWidth || r.srcHeight != r.dstHeight || r.srcLayers != r.dstLayers) {
        return std::format("{}: the resolve source is {}x{} with {} layer(s), the destination {}x{} with {}; they "
                           "must match",
                           call, r.srcWidth, r.srcHeight, r.srcLayers, r.dstWidth, r.dstHeight, r.dstLayers);
    }
    if (r.srcFormat != r.dstFormat) {
        return std::format("{}: the resolve source and destination formats differ", call);
    }
    if (!r.dstHasAttachmentUsage) {
        return std::format("{}: the resolve destination needs TextureUsage::{}", call,
                           isDepthFormat(r.dstFormat) ? "eDepthStencil" : "eRenderTarget");
    }
    if (!isDepthFormat(r.srcFormat)) {
        const bool integer = r.srcFormat == Format::eR32Uint || r.srcFormat == Format::eRG32Uint ||
                             r.srcFormat == Format::eRGBA32Uint;
        if (r.mode != ResolveMode::eAverage || integer) {
            return std::format("{}: color resolves average, and only non-integer formats (P76)", call);
        }
        return {};
    }
    if ((r.depthModes & resolveModeBit(r.mode)) == 0 ||
        (hasStencil(r.srcFormat) && (r.stencilModes & resolveModeBit(r.mode)) == 0)) {
        return std::format("{}: this resolve mode is not supported for this depth format on this GPU "
                           "(DeviceLimits::depthResolveModes, stencilResolveModes; P76)",
                           call);
    }
    return {};
}

CheckResult checkIndirect(const IndirectCheck &c, std::string_view call) {
    if (!c.argsUsage || (c.hasCount && !c.countUsage)) {
        return std::format("{}: indirect argument and count buffers need BufferUsage::eIndirect", call);
    }
    if (c.offset % 4 != 0 || c.offset + c.stride * c.maxCount > c.argsSize) {
        return std::format("{}: {} record(s) of {} bytes at offset {} do not fit in the {}-byte buffer (offsets are "
                           "multiples of 4)",
                           call, c.maxCount, c.stride, c.offset, c.argsSize);
    }
    if (c.hasCount && (c.countOffset % 4 != 0 || c.countOffset + 4 > c.countSize)) {
        return std::format("{}: the count at offset {} must be a 4-byte aligned uint32 inside its buffer", call,
                           c.countOffset);
    }
    if (c.dispatch && !c.countForDispatchAllowed && (c.maxCount != 1 || c.hasCount)) {
        return std::format("{}: indirect dispatches take one record and no count (Vulkan has no dispatch count)",
                           call);
    }
    return {};
}

CheckResult checkSampler(const SamplerDesc &desc, bool minMaxSupported) {
    if (desc.maxAnisotropy < 1 || desc.maxAnisotropy > 16) {
        return std::format("writeSamplers: maxAnisotropy is {}; it goes from 1 (off) to 16", desc.maxAnisotropy);
    }
    if (desc.minLod > desc.maxLod) {
        return std::format("writeSamplers: minLod {} is above maxLod {}", desc.minLod, desc.maxLod);
    }
    if (desc.reduction != Reduction::eAverage && desc.compare.has_value()) {
        return std::string("writeSamplers: a comparison sampler cannot also use a min or max reduction");
    }
    if (desc.reduction != Reduction::eAverage && !minMaxSupported) {
        return std::string("writeSamplers: Reduction::eMin and eMax need DeviceLimits::samplerMinMax");
    }
    return {};
}

CheckResult checkMemoryType(bool allowed, std::string_view call) {
    if (!allowed) {
        return std::format("{}: this memory's type cannot hold the resource; allocate the memory with "
                           "allocateMemory(device, kind, size, &requirements)",
                           call);
    }
    return {};
}

CheckResult checkIndirectScratch(const IndirectScratchCheck &c, std::string_view call) {
    if (c.requiredSize == 0) {
        return {};
    }
    if (!c.hasScratch) {
        return std::format("{}: this call needs {} bytes of scratch (indirectScratchRequirements)", call,
                           c.requiredSize);
    }
    if (!c.usage) {
        return std::format("{}: the scratch buffer needs BufferUsage::eIndirectScratch", call);
    }
    if (c.address % c.requiredAlignment != 0 || c.offset + c.requiredSize > c.scratchSize) {
        return std::format("{}: {} bytes of scratch at offset {} (aligned to {}) do not fit in the {}-byte buffer", call,
                           c.requiredSize, c.offset, c.requiredAlignment, c.scratchSize);
    }
    return {};
}

CheckResult checkBufferViewSize(uint64 size, uint32 stride, bool constant, uint64 maxViewSize,
                                uint64 maxConstantSize) {
    if (constant) {
        if (constantViewSize(size) > maxConstantSize) {
            return std::format("constant buffer view of {} bytes: this device allows {} "
                               "(DeviceLimits::maxConstantViewSize, never above 64 KiB)",
                               constantViewSize(size), maxConstantSize);
        }
        return {};
    }
    const uint64 element = stride == 0 ? 4 : stride;
    if (size / element > kMaxBufferViewElements) {
        return std::format("buffer view of {} elements of {} bytes: a view covers at most {} elements on every "
                           "backend (kMaxBufferViewElements, D3D12's limit); split it into several views",
                           size / element, element, kMaxBufferViewElements);
    }
    if (size > maxViewSize) {
        return std::format("buffer view of {} bytes: this device allows {} (DeviceLimits::maxBufferViewSize)", size,
                           maxViewSize);
    }
    return {};
}

CheckResult checkGroupCounts(uint32 x, uint32 y, uint32 z, bool mesh, std::string_view call) {
    if (x > kMaxGroupsPerDimension || y > kMaxGroupsPerDimension || z > kMaxGroupsPerDimension) {
        return std::format("{}: {} x {} x {} groups; each dimension is at most {} on every backend "
                           "(kMaxGroupsPerDimension); fold the count into a grid (Helpers::groupGrid)",
                           call, x, y, z, kMaxGroupsPerDimension);
    }
    const uint64 total = uint64{x} * y * z;
    if (mesh && total > kMaxMeshGroups) {
        return std::format("{}: {} groups in one mesh draw; at most {} on every backend (kMaxMeshGroups)", call,
                           total, kMaxMeshGroups);
    }
    return {};
}

CheckResult checkDeviceThread(std::thread::id deviceThread, std::string_view call) {
    const std::thread::id here = std::this_thread::get_id();
    if (here == deviceThread) {
        return {};
    }
    std::ostringstream threads;
    threads << "thread " << here << ", but the device thread is " << deviceThread;
    return std::format("{}: called from {} (the thread that called createDevice). Only the any-thread list "
                       "(pipeline and work-graph creation, workGraphEntry, wait, isReached, requirement and "
                       "size queries) may run on other threads (P106)",
                       call, threads.str());
}

} // namespace RI::Validation
