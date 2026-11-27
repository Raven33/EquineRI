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
