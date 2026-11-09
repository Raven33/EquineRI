// Rendering sections and draws on Metal (P39, P40, P75, P101, P103, P104, P105).
//
// Owns: cmdBeginRendering (attachments, or a size alone) and cmdEndRendering,
// with their resolves (P52, P76) and visibility pages (P115), cmdResolve,
// cmdSetViewport, cmdSetScissor, cmdSetStencilReference, cmdDraw,
// cmdSetIndexBuffer, cmdDrawIndexed, cmdDrawMesh, and the graphics half of
// cmdSetPipeline (bindGraphicsPipeline).
//
// Keeps: the graphics pipeline, index buffer, viewport, scissor and stencil
// reference in CommandListState: Metal keeps them per render encoder, and
// each section opens a new one (beginRenderEncoder), so RI sets them again.
//
// Watch out: a section is one MTL4RenderCommandEncoder from an
// MTL4RenderPassDescriptor. Color attachments are the view's own texture
// (its format, level 0, slice 0 or the volume's depth plane); depth and
// stencil are the whole texture at the view's mip and layer, both aspects
// when the format has stencil, as a D3D12 DSV. Read-only depth or stencil
// (P75) loads and stores its aspect: Metal has no read-only attachments, and
// stores of unchanged contents keep them. A section without attachments
// (P105) gives Metal the size and one raster sample. MSC's vertex shaders
// declare the draw arguments at argument-table index 4 (IRRuntimeDrawArgument
// or IRRuntimeDrawIndexedArgument, D3D12's layouts) and the index type at
// index 5; RI binds them as MSC's IRRuntimeDraw* helpers do with
// setVertexBytes, but Metal 4 has no setBytes, so the arguments go to command
// memory (P115) and index 5 points at DeviceState::uniforms. MSC computes
// SV_InstanceID as instance_id - base_instance, as D3D12 (it excludes the
// first instance), and SV_VertexID as vertex_id - base_vertex (probed): unlike
// D3D12, the first vertex and vertex offset do not show in SV_VertexID.
// Indexed strips always restart at 0xFFFF / 0xFFFFFFFF on Metal; D3D12 and
// Vulkan leave restart off. Barriers inside a section cannot wait for its fragment
// work (Commands.cpp). Metal wants scissors inside the attachments, so RI
// clips them to the section. Resolves at cmdEndRendering are store actions
// with a resolve texture (Metal resolves as the section ends, as D3D12
// resolves after it); cmdResolve has no Metal command of its own, so it is a
// render pass that loads the source, stores it and resolves it, one per
// layer (the source's samples are read and written again). Metal resolves
// depth with sample 0, min or max, stencil with sample 0 only
// (kDepthResolveModes, kStencilResolveModes). While occlusion pools exist,
// every section points its visibility buffer at a page of command memory
// (Queries.cpp has the queries).
//
// D3D12 counterpart: the section, draw and section-state parts of
// backends/d3d12/Rendering.cpp; Vulkan: backends/vulkan/Commands.cpp.

#include "DeviceState.hpp"

#include <RI/Validation/Checks.hpp>
#include <RI/Validation/Placement.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace RI::Metal {

namespace {

// MSC's draw arguments (metal_irconverter_runtime.h: IRRuntimeDrawArgument,
// IRRuntimeDrawIndexedArgument, the same layouts as D3D12's), bound at
// kDrawArgumentsIndex as the size of their union, IRRuntimeDrawParams.
struct DrawArguments {
    uint32 vertexCountPerInstance;
    uint32 instanceCount;
    uint32 startVertexLocation;
    uint32 startInstanceLocation;
};
struct DrawIndexedArguments {
    uint32 indexCountPerInstance;
    uint32 instanceCount;
    uint32 startIndexLocation;
    int32 baseVertexLocation;
    uint32 startInstanceLocation;
};
constexpr uint64 kDrawParamsSize = sizeof(DrawIndexedArguments); // sizeof(IRRuntimeDrawParams)

MTL::Texture *resourceOf(const TextureView &view) { return static_cast<MTL::Texture *>(view.internal.resource); }

MTL::LoadAction loadOf(LoadOp op) {
    switch (op) {
    case LoadOp::eLoad: return MTL::LoadActionLoad;
    case LoadOp::eClear: return MTL::LoadActionClear;
    case LoadOp::eDontCare: return MTL::LoadActionDontCare;
    }
    return MTL::LoadActionLoad;
}

MTL::StoreAction storeOf(StoreOp op) { return op == StoreOp::eStore ? MTL::StoreActionStore : MTL::StoreActionDontCare; }

// A store that also resolves (P52): the multisampled contents stay only if stored.
MTL::StoreAction resolvingStoreOf(MTL::StoreAction store) {
    return store == MTL::StoreActionStore ? MTL::StoreActionStoreAndMultisampleResolve
                                          : MTL::StoreActionMultisampleResolve;
}

MTL::MultisampleDepthResolveFilter depthFilterOf(ResolveMode mode) {
    switch (mode) {
    case ResolveMode::eMin: return MTL::MultisampleDepthResolveFilterMin;
    case ResolveMode::eMax: return MTL::MultisampleDepthResolveFilterMax;
    default: return MTL::MultisampleDepthResolveFilterSample0;
    }
}

[[maybe_unused]] Validation::ResolveCheck resolveCheckOf(const TextureView &src, const TextureView &dst,
                                                         ResolveMode mode) {
    const TextureUsage attachment = isDepthFormat(dst.format) ? TextureUsage::eDepthStencil : TextureUsage::eRenderTarget;
    return {.srcSamples = src.samples,
            .dstSamples = dst.samples,
            .srcWidth = src.width,
            .srcHeight = src.height,
            .srcLayers = src.internal.layerCount,
            .dstWidth = dst.width,
            .dstHeight = dst.height,
            .dstLayers = dst.internal.layerCount,
            .srcFormat = src.format,
            .dstFormat = dst.format,
            .dstHasAttachmentUsage = (dst.internal.textureUsage & attachment) != TextureUsage::eNone,
            .mode = mode,
            .depthModes = kDepthResolveModes,
            .stencilModes = kStencilResolveModes};
}

// Color resolves go through the views' own textures (as color attachments),
// depth and stencil through the whole texture at the view's mip and layer.
void resolveColorInto(MTL::RenderPassAttachmentDescriptor *attachment, const TextureView &dst, uint32 layer) {
    attachment->setResolveTexture(static_cast<MTL::Texture *>(dst.internal.texture));
    attachment->setResolveLevel(0);
    attachment->setResolveSlice(layer);
}

void resolveDepthInto(MTL::RenderPassDepthAttachmentDescriptor *depth, MTL::RenderPassStencilAttachmentDescriptor *stencil,
                      const TextureView &dst, ResolveMode mode, uint32 layer) {
    const auto into = [&](MTL::RenderPassAttachmentDescriptor *attachment) {
        attachment->setResolveTexture(resourceOf(dst));
        attachment->setResolveLevel(dst.internal.baseMip);
        attachment->setResolveSlice(dst.internal.baseLayer + layer);
    };
    into(depth);
    depth->setDepthResolveFilter(depthFilterOf(mode));
    if (stencil != nullptr) {
        into(stencil);
        stencil->setStencilResolveFilter(MTL::MultisampleStencilResolveFilterSample0);
    }
}

// While occlusion pools exist (P115): the section's visibility page, from
// command memory of its own (allocateVisibilityPage). Metal zeroes
// each slot a query names when the section begins (VisibilityResultTypeReset,
// probed: only those slots), so the page needs no clear.
void attachVisibilityPage(DeviceState &state, CommandListState &cmd, MTL4::RenderPassDescriptor *pass) {
    cmd.visibilityBuffer = nullptr;
    cmd.occlusionSlots.clear();
    cmd.occlusionOpen = false;
    if (state.occlusionPools.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const CommandMemoryAllocation page = allocateVisibilityPage(state, cmd);
    cmd.visibilityBuffer = page.buffer;
    cmd.visibilityOffset = page.offset;
    pass->setVisibilityResultBuffer(page.buffer);
    pass->setVisibilityResultType(MTL::VisibilityResultTypeReset);
}

// Every build: Metal faults on a render pass or draw that is not set up.
void require(bool ok, const char *call, const char *what) {
    if (!ok) {
        reportError(ErrorCategory::eProgrammerError, std::format("{}: {}", call, what));
    }
}

void requireOutsideSection(const CommandListState &cmd, const char *call) {
    if (const Validation::CheckResult r = Validation::checkRenderingSection(cmd.rendering, false, call)) {
        reportError(ErrorCategory::eProgrammerError, *r);
    }
}

void applyViewport(CommandListState &cmd) {
    const Viewport &v = cmd.viewport;
    cmd.render->setViewport(MTL::Viewport{.originX = v.x,
                                          .originY = v.y,
                                          .width = v.width,
                                          .height = v.height,
                                          .znear = v.minDepth,
                                          .zfar = v.maxDepth});
}

// Clipped to the section: Metal rejects scissors beyond the attachments.
void applyScissor(CommandListState &cmd) {
    const Rect &r = cmd.scissor;
    const uint64 x = std::min<uint64>(static_cast<uint64>(std::max(r.x, 0)), cmd.renderWidth);
    const uint64 y = std::min<uint64>(static_cast<uint64>(std::max(r.y, 0)), cmd.renderHeight);
    cmd.render->setScissorRect(MTL::ScissorRect{.x = x,
                                                .y = y,
                                                .width = std::min<uint64>(r.width, cmd.renderWidth - x),
                                                .height = std::min<uint64>(r.height, cmd.renderHeight - y)});
}

// The pipeline's render encoder state (P38: fixed per pipeline, encoder state on Metal).
void applyPipeline(CommandListState &cmd) {
    const auto &p = cmd.graphics.internal;
    MTL4::RenderCommandEncoder *encoder = cmd.render;
    encoder->setRenderPipelineState(static_cast<MTL::RenderPipelineState *>(p.pipelineState));
    encoder->setDepthStencilState(static_cast<MTL::DepthStencilState *>(p.depthStencilState));
    encoder->setCullMode(static_cast<MTL::CullMode>(p.cullMode));
    encoder->setFrontFacingWinding(static_cast<MTL::Winding>(p.winding));
    encoder->setTriangleFillMode(static_cast<MTL::TriangleFillMode>(p.fillMode));
    encoder->setDepthClipMode(static_cast<MTL::DepthClipMode>(p.depthClipMode));
    encoder->setDepthBias(p.depthBias[0], p.depthBias[1], p.depthBias[2]);
}

// The state every section starts with (P103, P104): viewport and scissor
// cover the section, the stencil reference is the pipeline's.
void openSection(CommandListState &cmd, uint32 width, uint32 height, bool depthReadOnly) {
    cmd.rendering = true;
    cmd.renderWidth = width;
    cmd.renderHeight = height;
    cmd.viewport = {.width = static_cast<float>(width), .height = static_cast<float>(height)};
    cmd.scissor = {.width = width, .height = height};
    cmd.stencilOverride = false;
    cmd.stencilReference = cmd.graphicsSet ? cmd.graphics.internal.stencilReference : 0;
    cmd.depthReadOnly = depthReadOnly;
    cmd.sectionDrawn = false;
    cmd.sectionMarkers.clear();
    cmd.deferredPops = 0;
    if (cmd.graphicsSet) {
        // A pipeline set before the section: Metal has no read-only
        // attachments and would store its depth writes (P75, P88).
        EQ_VALIDATE(Validation::checkDepthWriteAllowed(depthReadOnly, cmd.graphics.internal.depthWrite));
        applyPipeline(cmd);
    }
    applyViewport(cmd);
    applyScissor(cmd);
    cmd.render->setStencilReferenceValue(cmd.stencilReference);
}

// Draw arguments for MSC's vertex shaders (kDrawArgumentsIndex) and the index
// type (kUniformsIndex), read when the draw runs.
template <typename Arguments>
void bindDrawArguments(DeviceState &state, CommandListState &cmd, const Arguments &arguments, uint64 indexType) {
    const CommandMemoryAllocation memory = allocateCommandMemory(state, cmd, kDrawParamsSize, 16);
    std::memset(memory.cpu, 0, kDrawParamsSize);
    std::memcpy(memory.cpu, &arguments, sizeof(arguments));
    cmd.argumentTable->setAddress(memory.gpuAddress, kDrawArgumentsIndex);
    bindIndexType(state, cmd, indexType);
}

#if EQUINE_VALIDATION
void checkAttachmentDiscarded(const DeviceState &state, const TextureView &view) {
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, view.internal.textureId, "cmdBeginRendering"));
}
#endif

} // namespace

// ── Shared draw helpers (DeviceState.hpp) ────────────────────────────────────

void checkDraw(CommandListState &cmd, const char *call, bool mesh) {
    require(cmd.render != nullptr, call, "no rendering section is open (cmdBeginRendering)");
    require(cmd.graphicsSet, call, "no graphics pipeline is set (cmdSetPipeline)");
    if (cmd.graphics.internal.mesh != mesh) {
        reportError(ErrorCategory::eProgrammerError,
                    std::format("{}: the pipeline set is a {} pipeline; draw it with {}", call,
                                mesh ? "vertex" : "mesh", mesh ? "cmdDraw or cmdDrawIndexed" : "cmdDrawMesh"));
    }
}

void bindIndexType(DeviceState &state, CommandListState &cmd, uint64 indexType) {
    cmd.argumentTable->setAddress(state.uniforms->gpuAddress() + indexType * kIndexTypeStride, kUniformsIndex);
}

// ── Pipelines (cmdSetPipeline, Commands.cpp) ─────────────────────────────────

void bindGraphicsPipeline(CommandListState &cmd, const Pipeline &pipeline) {
    cmd.graphics = pipeline;
    cmd.graphicsSet = true;
    // The pipeline's reference, unless cmdSetStencilReference set one in this section (P104).
    if (!cmd.stencilOverride) {
        cmd.stencilReference = pipeline.internal.stencilReference;
    }
    if (cmd.render != nullptr) {
        applyPipeline(cmd);
        cmd.render->setStencilReferenceValue(cmd.stencilReference);
    }
}

// ── Sections (P40) ───────────────────────────────────────────────────────────

void cmdBeginRendering(CommandList &cmd, const RenderingDesc &desc) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    CommandListState &list = *cmd.internal.state;
    requireOutsideSection(list, "cmdBeginRendering");
    if (desc.colors.size() > 8) {
        reportError(ErrorCategory::eProgrammerError, "cmdBeginRendering: at most 8 color attachments");
    }
    if (const Validation::CheckResult r = Validation::checkRenderingAttachments(desc.colors.size(), desc.depth.has_value())) {
        reportError(ErrorCategory::eProgrammerError, *r);
    }
    uint32 width = ~0u;
    uint32 height = ~0u;
    uint32 layers = ~0u;
    const auto cover = [&](const TextureView &view) {
        width = std::min(width, view.width);
        height = std::min(height, view.height);
        layers = std::min(layers, view.internal.layerCount);
    };

    // The descriptor and its attachment descriptors are autoreleased or released here.
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL4::RenderPassDescriptor *pass = MTL4::RenderPassDescriptor::alloc()->init();
    for (size_t i = 0; i < desc.colors.size(); ++i) {
        const ColorAttachment &a = desc.colors[i];
        EQ_MTL_CHECK_ALIVE(&state, a.view.internal.textureId, "texture");
#if EQUINE_VALIDATION
        checkAttachmentDiscarded(state, a.view);
#endif
        require((a.view.internal.textureUsage & TextureUsage::eRenderTarget) != TextureUsage::eNone,
                "cmdBeginRendering", "a color attachment needs a texture with TextureUsage::eRenderTarget");
        MTL::RenderPassColorAttachmentDescriptor *color = pass->colorAttachments()->object(i);
        // The view's own texture: its format, from its first mip and layer.
        color->setTexture(static_cast<MTL::Texture *>(a.view.internal.texture));
        color->setLevel(0);
        if (a.view.internal.shape == TextureShape::e3D) {
            color->setDepthPlane(a.view.internal.baseLayer); // a volume view keeps every slice (P102)
        }
        color->setLoadAction(loadOf(a.load));
        color->setStoreAction(storeOf(a.store));
        // Resolved as the section ends (P52), after its last draw.
        if (a.resolveTo.format != Format::eUndefined) {
            EQ_MTL_CHECK_ALIVE(&state, a.resolveTo.internal.textureId, "texture");
            EQ_VALIDATE(Validation::checkResolve(resolveCheckOf(a.view, a.resolveTo, a.resolveMode), "cmdBeginRendering"));
            resolveColorInto(color, a.resolveTo, 0);
            color->setStoreAction(resolvingStoreOf(storeOf(a.store)));
        }
        color->setClearColor(MTL::ClearColor(a.clear.color[0], a.clear.color[1], a.clear.color[2], a.clear.color[3]));
        cover(a.view);
    }

    bool depthReadOnly = false;
    if (desc.depth) {
        const DepthAttachment &d = *desc.depth;
        EQ_MTL_CHECK_ALIVE(&state, d.view.internal.textureId, "texture");
#if EQUINE_VALIDATION
        checkAttachmentDiscarded(state, d.view);
#endif
        require((d.view.internal.textureUsage & TextureUsage::eDepthStencil) != TextureUsage::eNone,
                "cmdBeginRendering", "a depth attachment needs a texture with TextureUsage::eDepthStencil");
        // The texture itself, whose format has both aspects (a D3D12 DSV).
        const auto attach = [&](MTL::RenderPassAttachmentDescriptor *attachment, LoadOp load, StoreOp store,
                                bool readOnly) {
            attachment->setTexture(resourceOf(d.view));
            attachment->setLevel(d.view.internal.baseMip);
            attachment->setSlice(d.view.internal.baseLayer);
            // Read-only (P75): the contents stay, whatever the ops say.
            attachment->setLoadAction(readOnly ? MTL::LoadActionLoad : loadOf(load));
            attachment->setStoreAction(readOnly ? MTL::StoreActionStore : storeOf(store));
        };
        MTL::RenderPassDepthAttachmentDescriptor *depth = pass->depthAttachment();
        attach(depth, d.load, d.store, d.depthReadOnly);
        depth->setClearDepth(d.clear.depth);
        MTL::RenderPassStencilAttachmentDescriptor *stencil = nullptr;
        if (hasStencil(d.view.format)) {
            stencil = pass->stencilAttachment();
            attach(stencil, d.stencilLoad, d.stencilStore, d.stencilReadOnly);
            stencil->setClearStencil(d.clear.stencil);
        }
        // Depth and stencil with the same mode (P76), validated against what Metal has.
        if (d.resolveTo.format != Format::eUndefined) {
            EQ_MTL_CHECK_ALIVE(&state, d.resolveTo.internal.textureId, "texture");
            EQ_VALIDATE(Validation::checkResolve(resolveCheckOf(d.view, d.resolveTo, d.resolveMode), "cmdBeginRendering"));
            resolveDepthInto(depth, stencil, d.resolveTo, d.resolveMode, 0);
            depth->setStoreAction(resolvingStoreOf(depth->storeAction()));
            if (stencil != nullptr) {
                stencil->setStoreAction(resolvingStoreOf(stencil->storeAction()));
            }
        }
        depthReadOnly = d.depthReadOnly;
        cover(d.view);
    }
    // Layered rendering: the shader picks the layer (SV_RenderTargetArrayIndex).
    if (layers > 1) {
        pass->setRenderTargetArrayLength(layers);
    }
    attachVisibilityPage(state, list, pass);
    beginRenderEncoder(list, pass);
    pass->release();
    pool->release();
    openSection(list, width, height, depthReadOnly);
}

// A section without attachments (P105): Metal rasterizes the size given, one sample per pixel.
void cmdBeginRendering(CommandList &cmd, RenderingSize size) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    requireOutsideSection(list, "cmdBeginRendering");
    // Metal refuses a pass of size 0: every build.
    if (const Validation::CheckResult r = Validation::checkRenderingSize(size)) {
        reportError(ErrorCategory::eProgrammerError, *r);
    }
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL4::RenderPassDescriptor *pass = MTL4::RenderPassDescriptor::alloc()->init();
    pass->setRenderTargetWidth(size.width);
    pass->setRenderTargetHeight(size.height);
    pass->setDefaultRasterSampleCount(1);
    attachVisibilityPage(*cmd.internal.device, list, pass);
    beginRenderEncoder(list, pass);
    pass->release();
    pool->release();
    openSection(list, size.width, size.height, false);
}

void cmdEndRendering(CommandList &cmd) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    if (list.render == nullptr) {
        reportError(ErrorCategory::eProgrammerError, "cmdEndRendering: no rendering section is open");
    }
    // Every build: the section's results are copied out now, so the query
    // would count nothing (P115).
    if (list.occlusionOpen) {
        reportError(ErrorCategory::eProgrammerError,
                    "cmdEndRendering: an occlusion query is still open; on Metal, an occlusion query begins and "
                    "ends inside one rendering section (P115)");
    }
    // Markers begun in the section were the encoder's debug groups; still
    // open, they continue on the command buffer, so regions keep nesting.
    std::vector<std::string> open = std::move(list.sectionMarkers);
    list.sectionMarkers.clear();
    for (size_t i = 0; i < open.size(); ++i) {
        list.render->popDebugGroup();
    }
    endEncoder(list);
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    for (; list.deferredPops > 0; --list.deferredPops) {
        list.commandBuffer->popDebugGroup();
    }
    for (const std::string &name : open) {
        list.commandBuffer->pushDebugGroup(nsString(name));
    }
    pool->release();
    list.rendering = false;
    list.depthReadOnly = false;
    copyOcclusionResults(list);
}

// ── Section state (P103, P104) ───────────────────────────────────────────────

void cmdSetViewport(CommandList &cmd, const Viewport &viewport) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, true, "cmdSetViewport"));
    EQ_VALIDATE(Validation::checkViewport(viewport));
    list.viewport = viewport;
    if (list.render != nullptr) {
        applyViewport(list);
    }
}

void cmdSetScissor(CommandList &cmd, const Rect &scissor) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, true, "cmdSetScissor"));
    EQ_VALIDATE(Validation::checkScissor(scissor));
    list.scissor = scissor;
    if (list.render != nullptr) {
        applyScissor(list);
    }
}

void cmdSetStencilReference(CommandList &cmd, uint8 reference) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    EQ_VALIDATE(Validation::checkRenderingSection(list.rendering, true, "cmdSetStencilReference"));
    list.stencilReference = reference;
    list.stencilOverride = true;
    if (list.render != nullptr) {
        list.render->setStencilReferenceValue(reference);
    }
}

// ── Draws (P39) ──────────────────────────────────────────────────────────────

void cmdDraw(CommandList &cmd, uint32 vertexCount, uint32 instanceCount, uint32 firstVertex, uint32 firstInstance) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    checkDraw(list, "cmdDraw", false);
    if (vertexCount == 0 || instanceCount == 0) {
        return; // no work, as an empty draw on the other backends
    }
    bindDrawArguments(*cmd.internal.device, list,
                      DrawArguments{.vertexCountPerInstance = vertexCount,
                                    .instanceCount = instanceCount,
                                    .startVertexLocation = firstVertex,
                                    .startInstanceLocation = firstInstance},
                      kNonIndexed);
    // Only once a draw is encoded: Metal writes 0 for an after-fragment
    // timestamp in a section whose draws were all empty (cmdWriteTimestamp).
    list.sectionDrawn = true;
    list.render->drawPrimitives(static_cast<MTL::PrimitiveType>(list.graphics.internal.primitiveType), firstVertex,
                                vertexCount, instanceCount, firstInstance);
}

void cmdSetIndexBuffer(CommandList &cmd, const Buffer &buffer, uint64 offset, IndexFormat format) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_MTL_CHECK_ALIVE(cmd.internal.device, buffer.internal.id, "buffer");
    EQ_VALIDATE(Validation::checkIndexBuffer((buffer.usage & BufferUsage::eIndex) != BufferUsage::eNone, offset,
                                             format == IndexFormat::eUint16 ? 2 : 4, buffer.size));
    CommandListState &list = *cmd.internal.state;
    list.indexAddress = buffer.gpuAddress + offset;
    list.indexLength = offset < buffer.size ? buffer.size - offset : 0;
    list.indexFormat = format;
    list.indexSet = true;
}

void cmdDrawIndexed(CommandList &cmd, uint32 indexCount, uint32 instanceCount, uint32 firstIndex, int32 vertexOffset,
                    uint32 firstInstance) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    CommandListState &list = *cmd.internal.state;
    checkDraw(list, "cmdDrawIndexed", false);
    require(list.indexSet, "cmdDrawIndexed", "no index buffer is set (cmdSetIndexBuffer)");
    if (indexCount == 0 || instanceCount == 0) {
        return;
    }
    const bool wide = list.indexFormat == IndexFormat::eUint32;
    const uint64 first = uint64{firstIndex} * (wide ? 4 : 2);
    // MSC's helper writes the byte offset here; RI keeps D3D12's meaning (an
    // index count), as indirect records hold it. MSC's vertex shaders leave it
    // unread; its geometry and tessellation emulation reads it.
    bindDrawArguments(*cmd.internal.device, list,
                      DrawIndexedArguments{.indexCountPerInstance = indexCount,
                                           .instanceCount = instanceCount,
                                           .startIndexLocation = firstIndex,
                                           .baseVertexLocation = vertexOffset,
                                           .startInstanceLocation = firstInstance},
                      wide ? kIndexUint32 : kIndexUint16);
    list.sectionDrawn = true;
    list.render->drawIndexedPrimitives(static_cast<MTL::PrimitiveType>(list.graphics.internal.primitiveType),
                                       indexCount, wide ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16,
                                       list.indexAddress + first,
                                       first < list.indexLength ? list.indexLength - first : 0, instanceCount,
                                       vertexOffset, firstInstance);
}

// Metal launches object threadgroups (the task shader's) when the pipeline
// has one, mesh threadgroups otherwise; it takes both group sizes.
void cmdDrawMesh(CommandList &cmd, uint32 x, uint32 y, uint32 z) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    EQ_VALIDATE(Validation::checkGroupCounts(x, y, z, true, "cmdDrawMesh"));
    CommandListState &list = *cmd.internal.state;
    checkDraw(list, "cmdDrawMesh", true);
    if (x == 0 || y == 0 || z == 0) {
        return;
    }
    // MSC's mesh shaders read no draw arguments; the index type goes back to
    // "not indexed", as MSC's helpers leave it for every draw that is not indexed.
    bindIndexType(*cmd.internal.device, list, kNonIndexed);
    const auto &p = list.graphics.internal;
    list.sectionDrawn = true;
    list.render->drawMeshThreadgroups(
        MTL::Size(x, y, z), MTL::Size(p.taskThreadGroupSize[0], p.taskThreadGroupSize[1], p.taskThreadGroupSize[2]),
        MTL::Size(p.threadGroupSize[0], p.threadGroupSize[1], p.threadGroupSize[2]));
}

// ── Resolve outside sections (P52, P76) ──────────────────────────────────────

// Metal resolves only as a render pass stores: one pass per layer loads the
// source, stores it again and resolves it into the destination. Ordered as a
// clear pass (runClearPass): after all earlier work, before all later work.
void cmdResolve(CommandList &cmd, const TextureView &src, const TextureView &dst, ResolveMode mode) {
    EQ_MTL_CHECK_THREAD(cmd.internal.device);
    [[maybe_unused]] DeviceState &state = *cmd.internal.device;
    CommandListState &list = *cmd.internal.state;
    if (list.rendering) {
        reportError(ErrorCategory::eProgrammerError, "cmdResolve: not allowed inside a rendering section");
    }
    EQ_MTL_CHECK_ALIVE(&state, src.internal.textureId, "texture");
    EQ_MTL_CHECK_ALIVE(&state, dst.internal.textureId, "texture");
#if EQUINE_VALIDATION
    EQ_VALIDATE(Validation::checkDiscarded(state.placements, dst.internal.textureId, "cmdResolve"));
#endif
    EQ_VALIDATE(Validation::checkResolve(resolveCheckOf(src, dst, mode), "cmdResolve"));
    const bool depth = isDepthFormat(src.format);
    // Metal faults on a pass over an attachment without its usage: every build.
    require((src.internal.textureUsage & (depth ? TextureUsage::eDepthStencil : TextureUsage::eRenderTarget)) !=
                TextureUsage::eNone,
            "cmdResolve", depth ? "the source needs TextureUsage::eDepthStencil" : "the source needs TextureUsage::eRenderTarget");
    for (uint32 layer = 0; layer < src.internal.layerCount; ++layer) {
        NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
        MTL4::RenderPassDescriptor *pass = MTL4::RenderPassDescriptor::alloc()->init();
        if (depth) {
            MTL::RenderPassDepthAttachmentDescriptor *d = pass->depthAttachment();
            MTL::RenderPassStencilAttachmentDescriptor *s = hasStencil(src.format) ? pass->stencilAttachment() : nullptr;
            for (MTL::RenderPassAttachmentDescriptor *a : {static_cast<MTL::RenderPassAttachmentDescriptor *>(d),
                                                           static_cast<MTL::RenderPassAttachmentDescriptor *>(s)}) {
                if (a == nullptr) {
                    continue;
                }
                a->setTexture(resourceOf(src));
                a->setLevel(src.internal.baseMip);
                a->setSlice(src.internal.baseLayer + layer);
                a->setLoadAction(MTL::LoadActionLoad);
                a->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
            }
            resolveDepthInto(d, s, dst, mode, layer);
        } else {
            MTL::RenderPassColorAttachmentDescriptor *color = pass->colorAttachments()->object(0);
            color->setTexture(static_cast<MTL::Texture *>(src.internal.texture));
            color->setSlice(layer);
            color->setLoadAction(MTL::LoadActionLoad);
            color->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
            resolveColorInto(color, dst, layer);
        }
        runClearPass(list, pass);
        pass->release();
        pool->release();
    }
}

} // namespace RI::Metal
