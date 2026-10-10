// Portable GPU tests for textures, rendering, clears, discard and timestamps
// (Milestone 3). Every case checks texel values read back through a compute
// pass, so RI's layout handling (A', L1) is exercised end to end.

#include "GpuTestCommon.hpp"

#include <TestHarness.hpp>

#include <RI/RI.hpp>

#include <cmath>
#include <csignal>
#include <cstdlib>
#include <vector>

namespace {

using gputest::Arena;
using gputest::createTestDevice;
using gputest::loadShader;
using gputest::placeBuffer;
using gputest::placeTexture;

struct DrawRoot {
    float color[4];
    float depth;
};

struct ReadRoot {
    uint32 textureSlot;
    uint32 outputSlot;
    uint32 width;
    uint32 height;
    uint32 multisampled;
    uint32 sampleIndex;
};

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

constexpr uint32 kSize = 32;
constexpr uint32 kOutputSlot = 0;
constexpr uint32 kTextureSlot = 1;

// Everything a test needs: device, memory, a heap, an output buffer read back
// after the test's own commands, and the reading pipeline.
struct Fixture {
    RI::Device device;
    Arena gpu;
    Arena readback;
    RI::DescriptorHeap heap;
    RI::Buffer output;
    RI::Buffer result;
    std::vector<std::byte> readShader;
    RI::Pipeline read;
    RI::CommandList cmd;

    Fixture() {
        device = createTestDevice();
        gpu.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64ull << 20);
        readback.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
        heap = RI::createDescriptorHeap(device, {.resourceCount = 64});
        const uint64 bytes = kSize * kSize * 16;
        output = placeBuffer(device, gpu,
                             {.size = bytes, .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc});
        result = placeBuffer(device, readback, {.size = bytes, .usage = RI::BufferUsage::eCopyDst});
        RI::writeDescriptors(device, heap,
                             {{.slot = kOutputSlot, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 16}}});
        readShader = loadShader("read_texture");
        read = RI::createComputePipeline(device, {.shader = readShader});
        cmd = RI::createCommandList(device);
        RI::cmdBegin(cmd);
        RI::cmdSetHeap(cmd, heap);
    }

    // Reads `view` (sample `sample` if multisampled) into `result`, submits,
    // waits, and returns the texels.
    std::vector<float> readBack(const RI::TextureView &view, uint32 sample = 0) {
        RI::writeDescriptors(device, heap, {{.slot = kTextureSlot, .descriptor = RI::ReadView{.texture = view}}});
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCompute);
        RI::cmdSetPipeline(cmd, read);
        RI::cmdSetRootData(cmd, ReadRoot{.textureSlot = kTextureSlot,
                                         .outputSlot = kOutputSlot,
                                         .width = kSize,
                                         .height = kSize,
                                         .multisampled = view.samples > 1 ? 1u : 0u,
                                         .sampleIndex = sample});
        RI::cmdDispatch(cmd, kSize / 8, kSize / 8, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, output.size);
        RI::cmdEnd(cmd);
        RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
        const auto *texels = static_cast<const float *>(result.mapped);
        return {texels, texels + kSize * kSize * 4};
    }

    ~Fixture() {
        RI::destroyCommandList(device, cmd);
        RI::destroyPipeline(device, read);
        RI::destroyDescriptorHeap(device, heap);
        RI::destroyBuffer(device, output);
        RI::destroyBuffer(device, result);
        RI::freeMemory(device, gpu.memory);
        RI::freeMemory(device, readback.memory);
        RI::destroyDevice(device);
    }
};

bool near(float a, float b) { return std::fabs(a - b) < 1.0f / 128.0f; }

// Every texel's channel `channel` equals `value`.
bool all(const std::vector<float> &texels, uint32 channel, float value) {
    for (size_t i = channel; i < texels.size(); i += 4) {
        if (!near(texels[i], value)) {
            return false;
        }
    }
    return true;
}

RI::Pipeline drawPipeline(RI::Device &device, RI::Format color, RI::Format depth, uint32 samples, bool depthWrite,
                          std::vector<std::byte> &vs, std::vector<std::byte> &ps) {
    vs = loadShader("fullscreen_vs");
    ps = loadShader("solid_ps");
    RI::GraphicsPipelineDesc desc{.vertex = vs, .pixel = ps, .depthFormat = depth, .samples = samples};
    if (color != RI::Format::eUndefined) {
        desc.colorFormats = {color};
    } else {
        desc.pixel = {};
    }
    desc.depth = {.test = depth != RI::Format::eUndefined, .write = depthWrite, .compare = RI::CompareOp::eAlways};
    return RI::createGraphicsPipeline(device, desc);
}

} // namespace

// A texture created with each usage reports its facts and a whole view (P58, P73).
EQ_TEST(texture, create_and_views) {
    Fixture f;
    const RI::TextureDesc desc{.width = 64,
                               .height = 16,
                               .mips = 3,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget,
                               .viewFormats = {RI::Format::eRGBA8Srgb}};
    const RI::MemoryRequirements req = RI::textureRequirements(f.device, desc);
    EQ_CHECK(req.size > 0 && req.alignment > 0 && RI::fits(req, RI::MemoryKind::eGPU));
    RI::Texture texture = placeTexture(f.device, f.gpu, desc);
    EQ_CHECK(texture.width == 64 && texture.height == 16 && texture.mips == 3);
    EQ_CHECK(texture.view.format == RI::Format::eRGBA8Unorm && texture.view.width == 64);
    RI::TextureView mip1 = RI::createView(f.device, texture, {.format = RI::Format::eRGBA8Srgb, .baseMip = 1, .mipCount = 1});
    EQ_CHECK(mip1.width == 32 && mip1.height == 8 && mip1.format == RI::Format::eRGBA8Srgb);
    RI::destroyView(f.device, mip1);
    RI::destroyTexture(f.device, texture);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

// Render (clear + draw) -> sample, for several color formats (L1 item 2).
EQ_TEST(texture, render_then_sample) {
    Fixture f;
    for (RI::Format format : {RI::Format::eRGBA8Unorm, RI::Format::eRGBA16Float, RI::Format::eR11G11B10Float}) {
        if (!RI::formatSupported(f.device, format, RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget)) {
            continue;
        }
        RI::Texture texture = placeTexture(f.device, f.gpu,
                                           {.width = kSize,
                                            .height = kSize,
                                            .format = format,
                                            .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
        std::vector<std::byte> vs, ps;
        RI::Pipeline pipeline = drawPipeline(f.device, format, RI::Format::eUndefined, 1, false, vs, ps);
        RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eClear}}});
        RI::cmdSetPipeline(f.cmd, pipeline);
        RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.5f, 0.75f, 1.0f}, .depth = 0.5f});
        RI::cmdDraw(f.cmd, 3);
        RI::cmdEndRendering(f.cmd);
        const std::vector<float> texels = f.readBack(texture.view);
        EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 1, 0.5f) && all(texels, 2, 0.75f));
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyTexture(f.device, texture);
    }
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

// A cleared attachment that is not drawn to keeps its clear color (load op).
EQ_TEST(texture, clear_load_op) {
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view,
                                              .load = RI::LoadOp::eClear,
                                              .clear = {.color = {1.0f, 0.0f, 0.5f, 1.0f}}}}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(texture.view);
    EQ_CHECK(all(texels, 0, 1.0f) && all(texels, 1, 0.0f) && all(texels, 2, 0.5f));
    RI::destroyTexture(f.device, texture);
}

// Depth write -> sample (L1 item 4).
EQ_TEST(texture, depth_write_then_sample) {
    Fixture f;
    for (RI::Format format : {RI::Format::eD32Float, RI::Format::eD32FloatS8Uint, RI::Format::eD24UnormS8Uint}) {
        const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil;
        if (!RI::formatSupported(f.device, format, usage)) {
            continue;
        }
        RI::Texture depth = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format, .usage = usage});
        RI::TextureView depthOnly = RI::createView(f.device, depth, {.aspect = RI::Aspect::eDepth});
        std::vector<std::byte> vs, ps;
        RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eUndefined, format, 1, true, vs, ps);
        RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view,
                                                                    .load = RI::LoadOp::eClear,
                                                                    .stencilLoad = RI::LoadOp::eClear,
                                                                    .clear = {.depth = 1.0f}}});
        RI::cmdSetPipeline(f.cmd, pipeline);
        RI::cmdSetRootData(f.cmd, DrawRoot{.color = {}, .depth = 0.25f});
        RI::cmdDraw(f.cmd, 3);
        RI::cmdEndRendering(f.cmd);
        const std::vector<float> texels = f.readBack(depthOnly);
        EQ_CHECK(all(texels, 0, 0.25f));
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyView(f.device, depthOnly);
        RI::destroyTexture(f.device, depth);
    }
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
}

// Read-only depth: the depth texture is sampled while it is the depth attachment (P75).
EQ_TEST(texture, read_only_depth_section) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil;
    RI::Texture depth =
        placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eD32Float, .usage = usage});
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.75f}}});
    RI::cmdEndRendering(f.cmd);
    // A read-only section with no draw: the texture keeps its contents.
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(depth.view);
    EQ_CHECK(all(texels, 0, 0.75f));
    RI::destroyTexture(f.device, depth);
}

// 4x MSAA render -> sample one sample (L1 item 2, MSAA).
EQ_TEST(texture, msaa_render_then_sample) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget;
    if (!RI::formatSupported(f.device, RI::Format::eRGBA16Float, usage, 4)) {
        return;
    }
    RI::Texture texture = placeTexture(
        f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4, .format = RI::Format::eRGBA16Float, .usage = usage});
    EQ_CHECK(texture.view.samples == 4);
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eRGBA16Float, RI::Format::eUndefined, 4, false, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eDontCare}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.5f, 0.25f, 1.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(texture.view, 3);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 1, 0.25f));
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, texture);
}

// Storage write -> sample (L1 item 3).
EQ_TEST(texture, storage_write_then_sample) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eStorage;
    RI::Texture texture =
        placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float, .usage = usage});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = texture.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, WriteRoot{.textureSlot = 2, .width = kSize, .height = kSize, .value = 7.0f});
    RI::cmdDispatch(f.cmd, kSize / 8, kSize / 8, 1);
    const std::vector<float> texels = f.readBack(texture.view);
    EQ_CHECK(all(texels, 2, 7.0f));
    EQ_CHECK(near(texels[(5 * kSize + 3) * 4 + 0], 3.0f) && near(texels[(5 * kSize + 3) * 4 + 1], 5.0f));
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, texture);
}

// Clears outside rendering sections (P65): a render target, a storage texture,
// a depth texture, and a buffer.
EQ_TEST(texture, clear_commands) {
    Fixture f;
    RI::Texture color = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eRGBA8Unorm,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget});
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eStorage});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil});
    RI::cmdClearTexture(f.cmd, color.view, {.color = {0.0f, 1.0f, 0.0f, 1.0f}});
    RI::cmdClearTexture(f.cmd, storage.view, {.color = {2.0f, 3.0f, 4.0f, 5.0f}});
    RI::cmdClearTexture(f.cmd, depth.view, {.depth = 0.125f});
    std::vector<float> texels = f.readBack(color.view);
    EQ_CHECK(all(texels, 1, 1.0f) && all(texels, 0, 0.0f));

    for (const RI::Texture *t : {&storage, &depth}) {
        RI::resetCommandList(f.cmd);
        RI::cmdBegin(f.cmd);
        RI::cmdSetHeap(f.cmd, f.heap);
        texels = f.readBack(t->view);
        EQ_CHECK(t == &storage ? all(texels, 3, 5.0f) : all(texels, 0, 0.125f));
    }

    // A buffer clear, read back directly.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdClearBuffer(f.cmd, f.output, 0x3f800000u); // 1.0f
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy | RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, f.output.size);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *values = static_cast<const float *>(f.result.mapped);
    EQ_CHECK(values[0] == 1.0f && values[kSize * kSize * 4 - 1] == 1.0f);
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);

    RI::destroyTexture(f.device, color);
    RI::destroyTexture(f.device, storage);
    RI::destroyTexture(f.device, depth);
}

// A texture placed over another one's memory is discarded before use (P69).
EQ_TEST(texture, discard_after_aliasing) {
    Fixture f;
    const RI::TextureDesc desc{.width = kSize,
                               .height = kSize,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget};
    const uint64 offset = gputest::alignUp(f.gpu.used, RI::textureRequirements(f.device, desc).alignment);
    RI::Texture first = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdClearTexture(f.cmd, first.view, {.color = {1.0f, 1.0f, 1.0f, 1.0f}});
    RI::cmdBarrier(f.cmd, RI::Stage::eAll, RI::Stage::eAll);
    RI::destroyTexture(f.device, first);
    RI::Texture second = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdDiscard(f.cmd, second.view);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = second.view,
                                              .load = RI::LoadOp::eClear,
                                              .clear = {.color = {0.0f, 0.0f, 1.0f, 1.0f}}}}});
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(second.view);
    EQ_CHECK(all(texels, 2, 1.0f) && all(texels, 0, 0.0f));
    RI::destroyTexture(f.device, second);
}

// Timestamps (P51, P71): increasing ticks around GPU work; frequency reported.
EQ_TEST(texture, timestamps) {
    Fixture f;
    EQ_CHECK(f.device.limits.timestampFrequency > 0);
    RI::QueryPool pool = RI::createQueryPool(f.device, {.type = RI::QueryType::eTimestamp, .count = 4});
    Arena readback{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eReadback, 1 << 16)};
    RI::Buffer ticks = placeBuffer(f.device, readback, {.size = 32, .usage = RI::BufferUsage::eCopyDst});
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = 1024,
                                        .height = 1024,
                                        .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eRenderTarget});
    RI::cmdWriteTimestamp(f.cmd, pool, 0);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = texture.view, .load = RI::LoadOp::eClear}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdWriteTimestamp(f.cmd, pool, 1);
    RI::cmdResolveQueries(f.cmd, pool, 0, 2, ticks, 0);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *values = static_cast<const uint64 *>(ticks.mapped);
    EQ_CHECK(values[1] >= values[0] && values[0] != 0);
    RI::resetQueries(f.device, pool, 0, 2);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::destroyTexture(f.device, texture);
    RI::destroyBuffer(f.device, ticks);
    RI::freeMemory(f.device, readback.memory);
    RI::destroyQueryPool(f.device, pool);
}

// An unlisted view format is a readable error in every build (P74).
EQ_TEST(texture, unlisted_view_format_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize,
                                        .height = kSize,
                                        .format = RI::Format::eRGBA8Unorm,
                                        .usage = RI::TextureUsage::eSampled});
    RI::createView(f.device, texture, {.format = RI::Format::eRGBA8Srgb});
}

#if EQUINE_VALIDATION
// A depth-writing pipeline in a read-only depth section (P75).
EQ_TEST(texture, depth_write_in_read_only_section_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eUndefined, RI::Format::eD32Float, 1, true, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true}});
    RI::cmdSetPipeline(f.cmd, pipeline);
}

// A texture placed over used memory and rendered without discard (P69).
EQ_TEST(texture, missing_discard_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    const RI::TextureDesc desc{.width = kSize,
                               .height = kSize,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eRenderTarget};
    const uint64 offset = gputest::alignUp(f.gpu.used, RI::textureRequirements(f.device, desc).alignment);
    RI::Texture first = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::destroyTexture(f.device, first);
    RI::Texture second = RI::createTexture(f.device, f.gpu.memory, offset, desc);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = second.view, .load = RI::LoadOp::eClear}}});
}
#endif
