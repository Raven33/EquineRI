// Metal-only GPU tests: native access (P84), native sections (P118) and command memory (P115).
// Portable tests live in tests/gpu/.

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"

#include <RI/Helpers/TypedCommands.hpp>
#include <RI/RI.hpp>
#include <RI/Metal/Native.hpp>

#include <csignal>
#include <cstdlib>
#include <vector>

using gputest::Arena;
using gputest::createTestDevice;
using gputest::placeBuffer;
using gputest::placeTexture;

// Every accessor returns the object RI uses (P84).
EQ_TEST(metal, native_access) {
    RI::Device device = createTestDevice();
    const RI::Metal::NativeDevice n = RI::Metal::native(device);
    EQ_CHECK(n.device != nullptr && n.graphicsQueue != nullptr && n.residencySet != nullptr && n.timeline != nullptr);
    EQ_CHECK(n.graphicsQueue->device() == n.device);

    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20)};
    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 16)};
    EQ_CHECK(RI::Metal::native(gpu.memory) != nullptr && RI::Metal::native(gpu.memory)->size() >= (1 << 20));
    RI::Buffer buffer = placeBuffer(device, upload, {.size = 256, .usage = RI::BufferUsage::eRead});
    EQ_CHECK(RI::Metal::native(buffer)->gpuAddress() == buffer.gpuAddress);
    EQ_CHECK(RI::Metal::native(buffer)->contents() == buffer.mapped);
    RI::Texture texture = placeTexture(device, gpu, {.width = 8, .height = 4, .format = RI::Format::eRGBA8Unorm,
                                                     .usage = RI::TextureUsage::eSampled});
    EQ_CHECK(RI::Metal::native(texture)->width() == 8 && RI::Metal::native(texture)->height() == 4);
    EQ_CHECK(RI::Metal::native(texture.view)->pixelFormat() == MTL::PixelFormatRGBA8Unorm);

    RI::QueryPool timestamps = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 2});
    RI::QueryPool occlusion = RI::createQueryPool(device, {.type = RI::QueryType::eOcclusion, .count = 3});
    EQ_CHECK(RI::Metal::native(timestamps).counters != nullptr && RI::Metal::native(timestamps).results == nullptr);
    EQ_CHECK(RI::Metal::native(timestamps).counters->count() >= 2);
    EQ_CHECK(RI::Metal::native(occlusion).counters == nullptr && RI::Metal::native(occlusion).results != nullptr);
    EQ_CHECK(RI::Metal::native(occlusion).results->length() >= 3 * 8);

    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4, .samplerCount = 2});
    const RI::Metal::NativeDescriptorHeap h = RI::Metal::native(heap);
    EQ_CHECK(h.resources != nullptr && h.resources->length() >= 4 * 24);
    EQ_CHECK(h.samplers != nullptr && h.samplers->length() >= 2 * 24);

    const std::vector<std::byte> bytecode = gputest::loadShader("fill");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = bytecode});
    const RI::Metal::NativePipeline p = RI::Metal::native(pipeline);
    EQ_CHECK(p.compute != nullptr && p.render == nullptr && p.depthStencil == nullptr);

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    EQ_CHECK(RI::Metal::native(cmd) != nullptr);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyQueryPool(device, occlusion);
    RI::destroyQueryPool(device, timestamps);
    RI::destroyTexture(device, texture);
    RI::destroyBuffer(device, buffer);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// While an occlusion pool exists, each section takes a 32 KiB visibility page
// of command memory (P115): 64 sections in one list take 2 MiB of pages, and
// recording them again after a reset takes nothing more. Pages once shared the
// list's growing chunks, which then doubled about every 8 sections (63 MiB for
// 64 sections).
EQ_TEST(metal, visibility_pages_bounded) {
    RI::Device device = createTestDevice();
    const MTL::ResidencySet *residency = RI::Metal::native(device).residencySet;
    RI::QueryPool occlusion = RI::createQueryPool(device, {.type = RI::QueryType::eOcclusion, .count = 1});
    RI::CommandList cmd = RI::createCommandList(device);
    const auto record = [&] {
        RI::cmdBegin(cmd);
        for (int i = 0; i < 64; ++i) {
            RI::cmdBeginRendering(cmd, RI::RenderingSize{.width = 8, .height = 8});
            RI::cmdEndRendering(cmd);
        }
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        RI::resetCommandList(cmd);
        return residency->allocatedSize();
    };
    const uint64 before = residency->allocatedSize();
    const uint64 first = record();
    const uint64 second = record();
    EQ_CHECK(first - before <= 3ull << 20); // 2 MiB of pages and the first chunk
    EQ_CHECK(second == first);

    RI::destroyCommandList(device, cmd);
    RI::destroyQueryPool(device, occlusion);
    RI::destroyDevice(device);
}

// ── Native sections (P27, P70, P118) ─────────────────────────────────────────

namespace {

struct DrawRoot {
    float color[4];
    float depth;
};

// A native pipeline that draws a full-screen triangle in green, usable from an
// indirect command buffer (supportIndirectCommandBuffers), no buffers bound.
constexpr const char *kNativeSource = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 nativeVertex(uint v [[vertex_id]]) {
    const float2 uv = float2((v << 1) & 2, v & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
fragment float4 nativeFragment() { return float4(0, 1, 0, 1); }
)";

MTL::RenderPipelineState *nativeGreenPipeline(MTL::Device *device) {
    NS::Error *error = nullptr;
    MTL::Library *library = device->newLibrary(NS::String::string(kNativeSource, NS::UTF8StringEncoding), nullptr, &error);
    EQ_CHECK(library != nullptr);
    MTL::Function *vertex = library->newFunction(NS::String::string("nativeVertex", NS::UTF8StringEncoding));
    MTL::Function *fragment = library->newFunction(NS::String::string("nativeFragment", NS::UTF8StringEncoding));
    MTL::RenderPipelineDescriptor *desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vertex);
    desc->setFragmentFunction(fragment);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    desc->setSupportIndirectCommandBuffers(true);
    MTL::RenderPipelineState *pipeline = device->newRenderPipelineState(desc, &error);
    EQ_CHECK(pipeline != nullptr);
    desc->release();
    fragment->release();
    vertex->release();
    library->release();
    return pipeline;
}

} // namespace

// Outside a section, native code records its own compute encoder, ordered by
// cmdBarrier against RI's work on both sides (one barrier pending with no
// encoder open, as after a marker). Inside a section, native code draws in the
// section's encoder through its own indirect command buffer (P116 (3)), in its
// own residency set (P118 (5)), and disturbs the encoder's state: argument
// table, viewport, scissor, cull and fill mode. After endNative, RI's draw
// uses RI's pipeline, root data, viewport and scissor again.
EQ_TEST(metal, native_section) {
    RI::Device device = createTestDevice();
    const RI::Metal::NativeDevice nd = RI::Metal::native(device);
    Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 4 << 20)};
    Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::Buffer filled = placeBuffer(device, gpu, {.size = 256, .usage = RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst});
    RI::Buffer filledBack = placeBuffer(device, readback, {.size = 256, .usage = RI::BufferUsage::eCopyDst});
    RI::Texture target = placeTexture(device, gpu, {.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                                                    .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eCopySrc});
    const RI::CopyLayout layout = RI::copyLayout(device, target.view);
    RI::Buffer targetBack = placeBuffer(device, readback, {.size = layout.size, .usage = RI::BufferUsage::eCopyDst});
    const std::vector<std::byte> vs = gputest::loadShader("fullscreen_vs"), ps = gputest::loadShader("solid_ps");
    RI::Pipeline red = RI::createGraphicsPipeline(device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});

    // Native objects: a pipeline, a one-draw ICB in a residency set of its own, an argument table.
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    MTL::RenderPipelineState *green = nativeGreenPipeline(nd.device);
    MTL::IndirectCommandBufferDescriptor *icbDesc = MTL::IndirectCommandBufferDescriptor::alloc()->init();
    icbDesc->setCommandTypes(MTL::IndirectCommandTypeDraw);
    icbDesc->setInheritPipelineState(true);
    icbDesc->setInheritBuffers(false);
    icbDesc->setMaxVertexBufferBindCount(0);
    icbDesc->setMaxFragmentBufferBindCount(0);
    MTL::IndirectCommandBuffer *icb = nd.device->newIndirectCommandBuffer(icbDesc, 1, MTL::ResourceStorageModeShared);
    icbDesc->release();
    EQ_CHECK(icb != nullptr);
    icb->indirectRenderCommand(0)->drawPrimitives(MTL::PrimitiveTypeTriangle, 0, 3, 1, 0);
    NS::Error *error = nullptr;
    MTL::ResidencySetDescriptor *residencyDesc = MTL::ResidencySetDescriptor::alloc()->init();
    MTL::ResidencySet *residency = nd.device->newResidencySet(residencyDesc, &error);
    residencyDesc->release();
    residency->addAllocation(icb);
    residency->commit();
    nd.graphicsQueue->addResidencySet(residency);
    MTL4::ArgumentTableDescriptor *tableDesc = MTL4::ArgumentTableDescriptor::alloc()->init();
    tableDesc->setMaxBufferBindCount(1);
    MTL4::ArgumentTable *table = nd.device->newArgumentTable(tableDesc, &error);
    tableDesc->release();
    EQ_CHECK(table != nullptr);

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    // Outside a section: RI, native, RI on the same bytes.
    RI::cmdClearBuffer(cmd, filled, 0x11111111u);
    RI::cmdBeginMarker(cmd, "native compute"); // ends RI's encoder: the barrier below is pending
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eAll);
    {
        const RI::Metal::NativeCommands n = RI::Metal::beginNative(cmd);
        EQ_CHECK(n.commandBuffer == RI::Metal::native(cmd) && n.renderEncoder == nullptr);
        MTL4::ComputeCommandEncoder *encoder = n.commandBuffer->computeCommandEncoder();
        encoder->fillBuffer(RI::Metal::native(filled), NS::Range(0, 128), 0x5A);
        encoder->endEncoding();
        RI::Metal::endNative(cmd);
    }
    RI::cmdEndMarker(cmd);
    RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
    RI::cmdClearBuffer(cmd, filled, 0x22222222u, 64, 64);
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyBuffer(cmd, filled, 0, filledBack, 0, 256);

    // Inside a section: RI's red on the left half, the native ICB's green on the right.
    RI::cmdBeginRendering(cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(cmd, red);
    RI::Helpers::cmdSetRootData(cmd, DrawRoot{.color = {1, 0, 0, 1}, .depth = 0.5f});
    RI::cmdSetScissor(cmd, {.x = 0, .y = 0, .width = 8, .height = 16});
    {
        const RI::Metal::NativeCommands n = RI::Metal::beginNative(cmd);
        EQ_CHECK(n.commandBuffer == nullptr && n.renderEncoder != nullptr);
        MTL4::RenderCommandEncoder *encoder = n.renderEncoder;
        encoder->setRenderPipelineState(green);
        encoder->setViewport(MTL::Viewport{.originX = 8, .originY = 0, .width = 8, .height = 16, .znear = 0, .zfar = 1});
        encoder->setScissorRect(MTL::ScissorRect{.x = 8, .y = 0, .width = 8, .height = 16});
        encoder->executeCommandsInBuffer(icb, NS::Range(0, 1));
        encoder->setArgumentTable(table, MTL::RenderStageVertex | MTL::RenderStageFragment);
        encoder->setCullMode(MTL::CullModeFront);
        encoder->setTriangleFillMode(MTL::TriangleFillModeLines);
        RI::Metal::endNative(cmd);
    }
    RI::cmdDraw(cmd, 3);
    RI::cmdEndRendering(cmd);
    RI::cmdBarrier(cmd, RI::Stage::eRenderTarget, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(cmd, target.view, targetBack, 0);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    const auto *bytes = static_cast<const uint8 *>(filledBack.mapped);
    uint32 wrong = 0;
    for (uint32 i = 0; i < 256; ++i) {
        wrong += bytes[i] != (i < 64 ? 0x5A : i < 128 ? 0x22 : 0x11);
    }
    EQ_CHECK(wrong == 0);
    wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        const auto *row = reinterpret_cast<const uint32 *>(static_cast<const uint8 *>(targetBack.mapped) + y * layout.rowPitch);
        for (uint32 x = 0; x < 16; ++x) {
            wrong += row[x] != (x < 8 ? 0xFF0000FFu : 0xFF00FF00u); // RGBA8 little endian: red, green
        }
    }
    EQ_CHECK(wrong == 0);

    RI::destroyCommandList(device, cmd);
    nd.graphicsQueue->removeResidencySet(residency);
    table->release();
    residency->release();
    icb->release();
    green->release();
    pool->release();
    RI::destroyPipeline(device, red);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, targetBack);
    RI::destroyTexture(device, target);
    RI::destroyBuffer(device, filledBack);
    RI::destroyBuffer(device, filled);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// Misuse of native sections, reported in every build.
EQ_TEST(metal, native_section_nested_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::Metal::beginNative(cmd);
    RI::Metal::beginNative(cmd);
}

EQ_TEST(metal, end_native_without_begin_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::Metal::endNative(cmd);
}

EQ_TEST(metal, end_in_native_section_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::Metal::beginNative(cmd);
    RI::cmdEnd(cmd);
}

// ── Import (P77, P119) ───────────────────────────────────────────────────────

namespace {

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

} // namespace

// Native heaps, a buffer and a texture, imported and used by RI's commands;
// an imported compute pipeline (RI's own, imported again) writes the imported
// texture through a raw heap entry; an imported graphics pipeline draws.
// Importing joins RI's residency set, destroying the RI objects only leaves it:
// the native objects stay valid.
EQ_TEST(metal, import_objects) {
    RI::Device device = createTestDevice();
    const RI::Metal::NativeDevice nd = RI::Metal::native(device);
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    const uint64 residentBefore = nd.residencySet->allocationCount();

    // A shared placement heap as eReadback memory, a private one as eGPU memory.
    MTL::HeapDescriptor *heapDesc = MTL::HeapDescriptor::alloc()->init();
    heapDesc->setType(MTL::HeapTypePlacement);
    heapDesc->setSize(1 << 20);
    heapDesc->setResourceOptions(MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    MTL::Heap *sharedHeap = nd.device->newHeap(heapDesc);
    heapDesc->setResourceOptions(MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked);
    MTL::Heap *privateHeap = nd.device->newHeap(heapDesc);
    heapDesc->release();
    EQ_CHECK(sharedHeap != nullptr && privateHeap != nullptr);
    RI::Memory readbackMemory = RI::Metal::importMemory(device, sharedHeap, RI::MemoryKind::eReadback);
    RI::Memory gpuMemory = RI::Metal::importMemory(device, privateHeap, RI::MemoryKind::eGPU);
    EQ_CHECK(readbackMemory.mapped != nullptr && gpuMemory.mapped == nullptr);
    EQ_CHECK(RI::Metal::native(readbackMemory) == sharedHeap);

    // A native shared buffer as the copy source, a native private texture as storage.
    MTL::Buffer *nativeBuffer = nd.device->newBuffer(4096, MTL::ResourceStorageModeShared);
    RI::Buffer staging = RI::Metal::importBuffer(device, nativeBuffer, {.size = 4096, .usage = RI::BufferUsage::eCopySrc});
    EQ_CHECK(staging.mapped == nativeBuffer->contents() && staging.gpuAddress == nativeBuffer->gpuAddress());
    auto *pattern = static_cast<uint8 *>(staging.mapped);
    for (uint32 i = 0; i < 4096; ++i) {
        pattern[i] = static_cast<uint8>(i * 7);
    }
    MTL::TextureDescriptor *texDesc =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA32Float, 16, 16, false);
    texDesc->setStorageMode(MTL::StorageModePrivate);
    texDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    MTL::Texture *nativeTexture = nd.device->newTexture(texDesc);
    const RI::TextureDesc storageDesc{.width = 16, .height = 16, .format = RI::Format::eRGBA32Float,
                                      .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eCopySrc};
    RI::Texture storage = RI::Metal::importTexture(device, nativeTexture, storageDesc);
    EQ_CHECK(RI::Metal::native(storage) == nativeTexture);

    // RI buffers and a render target in the imported heaps.
    const RI::CopyLayout storageLayout = RI::copyLayout(device, storage.view);
    RI::Buffer gpuCopy = RI::createBuffer(device, gpuMemory, 0, {.size = 4096, .usage = RI::BufferUsage::eCopySrc | RI::BufferUsage::eCopyDst});
    const RI::TextureDesc targetDesc{.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                                     .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eCopySrc};
    const RI::MemoryRequirements targetNeeds = RI::textureRequirements(device, targetDesc);
    const uint64 targetOffset = (4096 + targetNeeds.alignment - 1) / targetNeeds.alignment * targetNeeds.alignment;
    RI::Texture target = RI::createTexture(device, gpuMemory, targetOffset, targetDesc);
    const RI::CopyLayout targetLayout = RI::copyLayout(device, target.view);
    RI::Buffer back = RI::createBuffer(device, readbackMemory, 0, {.size = 4096, .usage = RI::BufferUsage::eCopyDst});
    RI::Buffer storageBack = RI::createBuffer(device, readbackMemory, 64 << 10, {.size = storageLayout.size, .usage = RI::BufferUsage::eCopyDst});
    RI::Buffer targetBack = RI::createBuffer(device, readbackMemory, 128 << 10, {.size = targetLayout.size, .usage = RI::BufferUsage::eCopyDst});

    // RI's pipelines, imported a second time; the storage texture through a raw entry.
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::Metal::writeNativeDescriptor(device, heap, 3, {.textureViewID = nativeTexture->gpuResourceID()._impl});
    const std::vector<std::byte> cs = gputest::loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(device, {.shader = cs});
    const auto *groups = write.internal.threadGroupSize;
    RI::Pipeline importedWrite = RI::Metal::importPipeline(device, RI::Metal::native(write).compute, write.rootDataSize,
                                                          MTL::Size(groups[0], groups[1], groups[2]));
    const std::vector<std::byte> vs = gputest::loadShader("fullscreen_vs"), ps = gputest::loadShader("solid_ps");
    RI::Pipeline red = RI::createGraphicsPipeline(device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::Pipeline importedRed = RI::Metal::importPipeline(device, RI::Metal::native(red).render, red.rootDataSize,
                                                        {.depthStencil = RI::Metal::native(red).depthStencil});
    EQ_CHECK(importedWrite.rootDataSize == sizeof(WriteRoot) && importedRed.rootDataSize == sizeof(DrawRoot));

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdCopyBuffer(cmd, staging, 0, gpuCopy, 0, 4096);
    RI::cmdSetPipeline(cmd, importedWrite);
    RI::Helpers::cmdSetRootData(cmd, WriteRoot{.textureSlot = 3, .width = 16, .height = 16, .value = 9.0f});
    RI::cmdDispatch(cmd, 2, 2, 1);
    RI::cmdBarrier(cmd, RI::Stage::eCopy | RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(cmd, gpuCopy, 0, back, 0, 4096);
    RI::cmdCopyTextureToBuffer(cmd, storage.view, storageBack, 0);
    RI::cmdBeginRendering(cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(cmd, importedRed);
    RI::Helpers::cmdSetRootData(cmd, DrawRoot{.color = {1, 0, 0, 1}, .depth = 0.5f});
    RI::cmdDraw(cmd, 3);
    RI::cmdEndRendering(cmd);
    RI::cmdBarrier(cmd, RI::Stage::eRenderTarget, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(cmd, target.view, targetBack, 0);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    uint32 wrong = 0;
    const auto *copied = static_cast<const uint8 *>(back.mapped);
    for (uint32 i = 0; i < 4096; ++i) {
        wrong += copied[i] != static_cast<uint8>(i * 7);
    }
    EQ_CHECK(wrong == 0);
    wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        const auto *row = reinterpret_cast<const float *>(static_cast<const uint8 *>(storageBack.mapped) + y * storageLayout.rowPitch);
        const auto *pixels = reinterpret_cast<const uint32 *>(static_cast<const uint8 *>(targetBack.mapped) + y * targetLayout.rowPitch);
        for (uint32 x = 0; x < 16; ++x) {
            wrong += row[x * 4] != float(x) || row[x * 4 + 1] != float(y) || row[x * 4 + 2] != 9.0f;
            wrong += pixels[x] != 0xFF0000FFu;
        }
    }
    EQ_CHECK(wrong == 0);

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, importedRed); // forgets only
    RI::destroyPipeline(device, importedWrite);
    EQ_CHECK(RI::Metal::native(red).render->retainCount() >= 1);
    RI::destroyPipeline(device, red);
    RI::destroyPipeline(device, write);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, targetBack);
    RI::destroyBuffer(device, storageBack);
    RI::destroyBuffer(device, back);
    RI::destroyTexture(device, target);
    RI::destroyBuffer(device, gpuCopy);
    RI::destroyTexture(device, storage); // releases RI's view, forgets the texture
    RI::destroyBuffer(device, staging);  // forgets only
    RI::freeMemory(device, gpuMemory);
    RI::freeMemory(device, readbackMemory);
    // Nothing imported is left in RI's set once a submission commits it, and
    // the native objects still work.
    RI::CommandList empty = RI::createCommandList(device);
    RI::cmdBegin(empty);
    RI::cmdEnd(empty);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, empty));
    RI::destroyCommandList(device, empty);
    EQ_CHECK(nd.residencySet->allocationCount() == residentBefore);
    EQ_CHECK(nativeTexture->width() == 16 && static_cast<uint8 *>(nativeBuffer->contents())[1] == 7);
    EQ_CHECK(sharedHeap->size() >= (1 << 20) && privateHeap->usedSize() == 0);
    nativeTexture->release();
    nativeBuffer->release();
    privateHeap->release();
    sharedHeap->release();
    pool->release();
    RI::destroyDevice(device);
}

// A texture that is not what its description says, reported in every build.
EQ_TEST(metal, import_mismatched_texture_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 16, 16, false);
    desc->setStorageMode(MTL::StorageModePrivate);
    desc->setUsage(MTL::TextureUsageShaderRead);
    MTL::Texture *texture = RI::Metal::native(device).device->newTexture(desc);
    RI::Metal::importTexture(device, texture, {.width = 32, .height = 16, .format = RI::Format::eRGBA8Unorm,
                                               .usage = RI::TextureUsage::eSampled});
}
