// Portable GPU tests for textures, rendering, clears, discard and timestamps
// (Milestone 3). Every case checks texel values read back through a compute
// pass, so RI's layout handling (A', L1) is exercised end to end.

#include "GpuTestCommon.hpp"
#include "TextureChecks.hpp"

#include <TestHarness.hpp>

#include <RI/Helpers/Statistics.hpp>
#include <RI/RI.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
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

    bool ownsDevice = true;

    Fixture() : Fixture(createTestDevice(), true) {}

    // `owns`: the fixture destroys the device at the end.
    Fixture(RI::Device existing, bool owns) {
        device = existing;
        ownsDevice = owns;
        gpu.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 64ull << 20);
        readback.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20);
        heap = RI::createDescriptorHeap(device, {.resourceCount = 64});
        const uint64 bytes = kSize * kSize * 16;
        output = placeBuffer(device, gpu,
                             {.size = bytes,
                              .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc |
                                       RI::BufferUsage::eCopyDst});
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
        if (ownsDevice) {
            RI::destroyDevice(device);
        }
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
void gputest::renderThenSample(RI::Device &device) {
    Fixture f(device, false);
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

EQ_TEST(texture, render_then_sample) {
    RI::Device device = createTestDevice();
    gputest::renderThenSample(device);
    RI::destroyDevice(device);
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
void gputest::depthWriteThenSample(RI::Device &device) {
    Fixture f(device, false);
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

EQ_TEST(texture, depth_write_then_sample) {
    RI::Device device = createTestDevice();
    gputest::depthWriteThenSample(device);
    RI::destroyDevice(device);
}

// Read-only depth (P75): a section keeps the depth contents, with and without
// eReadOnlyDepthSampling (P97: without it, Vulkan mode T switches the texture
// in and out of the section).
void readOnlyDepthSection(RI::TextureUsage extra) {
    Fixture f;
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil | extra;
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

EQ_TEST(texture, read_only_depth_section) { readOnlyDepthSection(RI::TextureUsage::eNone); }

EQ_TEST(texture, read_only_depth_section_sampling) {
    readOnlyDepthSection(RI::TextureUsage::eReadOnlyDepthSampling);
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
void gputest::storageWriteThenSample(RI::Device &device) {
    Fixture f(device, false);
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

EQ_TEST(texture, storage_write_then_sample) {
    RI::Device device = createTestDevice();
    gputest::storageWriteThenSample(device);
    RI::destroyDevice(device);
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
    // Destruction is immediate (P20): only after the GPU is done with `first`.
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    RI::destroyTexture(f.device, first);
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
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
// eReadOnlyDepthSampling on a color texture (P97).
EQ_TEST(texture, read_only_depth_sampling_on_color_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    placeTexture(f.device, f.gpu,
                 {.width = kSize,
                  .height = kSize,
                  .format = RI::Format::eRGBA8Unorm,
                  .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eReadOnlyDepthSampling});
}

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

namespace {

// The value of byte `c` of texel (x, y) in `layer` for the copy tests.
uint8 copyPattern(uint32 x, uint32 y, uint32 layer, uint32 c) { return static_cast<uint8>(x + y * 3 + layer * 7 + c * 11); }

} // namespace

// Buffer -> texture -> texture -> buffer, on one mip of a two-layer texture
// (P47): the bytes come back where copyLayout says.
EQ_TEST(texture, copy_round_trip) {
    Fixture f;
    const RI::TextureDesc desc{.width = 64,
                               .height = 48,
                               .layers = 2,
                               .mips = 2,
                               .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                        RI::TextureUsage::eCopyDst};
    RI::Texture a = placeTexture(f.device, f.gpu, desc);
    RI::Texture b = placeTexture(f.device, f.gpu, desc);
    RI::TextureView aMip = RI::createView(f.device, a, {.baseMip = 1, .mipCount = 1});
    RI::TextureView bMip = RI::createView(f.device, b, {.baseMip = 1, .mipCount = 1});
    const RI::CopyLayout layout = RI::copyLayout(f.device, aMip);
    EQ_CHECK(layout.width == 32 && layout.height == 24 && layout.layers == 2 && layout.bytesPerTexel == 4);
    EQ_CHECK(layout.rowPitch >= 32 * 4 && layout.layerPitch >= layout.rowPitch * 24);
    EQ_CHECK(layout.layerPitch % layout.alignment == 0);

    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)};
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    RI::Buffer back = placeBuffer(f.device, f.readback, {.size = layout.size, .usage = RI::BufferUsage::eCopyDst});
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint32 layer = 0; layer < 2; ++layer) {
        for (uint32 y = 0; y < 24; ++y) {
            for (uint32 x = 0; x < 32; ++x) {
                for (uint32 c = 0; c < 4; ++c) {
                    bytes[layer * layout.layerPitch + y * layout.rowPitch + x * 4 + c] = copyPattern(x, y, layer, c);
                }
            }
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, aMip);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTexture(f.cmd, aMip, bMip);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(f.cmd, bMip, back, 0);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    const auto *out = static_cast<const uint8 *>(back.mapped);
    uint32 wrong = 0;
    for (uint32 layer = 0; layer < 2; ++layer) {
        for (uint32 y = 0; y < 24; ++y) {
            for (uint32 x = 0; x < 32; ++x) {
                for (uint32 c = 0; c < 4; ++c) {
                    wrong += out[layer * layout.layerPitch + y * layout.rowPitch + x * 4 + c] !=
                             copyPattern(x, y, layer, c);
                }
            }
        }
    }
    EQ_CHECK(wrong == 0);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyBuffer(f.device, staging);
    RI::destroyBuffer(f.device, back);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyView(f.device, aMip);
    RI::destroyView(f.device, bMip);
    RI::destroyTexture(f.device, a);
    RI::destroyTexture(f.device, b);
}

// Depth values uploaded with a copy are what a shader then samples (P47).
EQ_TEST(texture, copy_depth_upload) {
    Fixture f;
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize,
                                      .height = kSize,
                                      .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil |
                                               RI::TextureUsage::eCopyDst});
    const RI::CopyLayout layout = RI::copyLayout(f.device, depth.view);
    EQ_CHECK(layout.bytesPerTexel == 4);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)};
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint32 y = 0; y < kSize; ++y) {
        auto *row = reinterpret_cast<float *>(bytes + y * layout.rowPitch);
        for (uint32 x = 0; x < kSize; ++x) {
            row[x] = 0.25f;
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, depth.view);
    const std::vector<float> texels = f.readBack(depth.view);
    EQ_CHECK(all(texels, 0, 0.25f));
    RI::destroyBuffer(f.device, staging);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyTexture(f.device, depth);
}

// Occlusion counts every covered sample; pipeline statistics count draws and
// dispatches at the offsets pipelineStatisticsLayout gives (P71).
EQ_TEST(texture, occlusion_and_statistics) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize,
                                       .height = kSize,
                                       .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, RI::Format::eRGBA8Unorm, RI::Format::eUndefined, 1, false, vs, ps);
    RI::QueryPool occlusion = RI::createQueryPool(f.device, {.type = RI::QueryType::eOcclusion, .count = 1});
    RI::QueryPool statistics = RI::createQueryPool(f.device, {.type = RI::QueryType::ePipelineStatistics, .count = 2});
    const RI::PipelineStatisticsLayout stats = RI::pipelineStatisticsLayout(f.device);
    RI::Buffer results =
        placeBuffer(f.device, f.readback, {.size = 8 + 2 * uint64{stats.size}, .usage = RI::BufferUsage::eCopyDst});

    // Inside the section: the clear at cmdBeginRendering is GPU work that
    // drivers may run as compute, which statistics would count.
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdBeginQuery(f.cmd, statistics, 0);
    RI::cmdBeginQuery(f.cmd, occlusion, 0);
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndQuery(f.cmd, occlusion, 0);
    RI::cmdEndQuery(f.cmd, statistics, 0);
    RI::cmdEndRendering(f.cmd);

    RI::writeDescriptors(f.device, f.heap, {{.slot = kTextureSlot, .descriptor = RI::ReadView{.texture = target.view}}});
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eCompute);
    RI::cmdBeginQuery(f.cmd, statistics, 1);
    RI::cmdSetPipeline(f.cmd, f.read);
    RI::cmdSetRootData(f.cmd, ReadRoot{.textureSlot = kTextureSlot, .outputSlot = kOutputSlot, .width = kSize,
                                       .height = kSize, .multisampled = 0, .sampleIndex = 0});
    RI::cmdDispatch(f.cmd, kSize / 8, kSize / 8, 1);
    RI::cmdEndQuery(f.cmd, statistics, 1);
    RI::cmdResolveQueries(f.cmd, occlusion, 0, 1, results, 0);
    RI::cmdResolveQueries(f.cmd, statistics, 0, 2, results, 8);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    const auto *base = static_cast<const uint8 *>(results.mapped);
    const auto value = [&](uint64 offset) { return *reinterpret_cast<const uint64 *>(base + offset); };
    EQ_CHECK(value(0) == kSize * kSize);
    const uint64 draw = 8;
    const uint64 dispatch = 8 + stats.size;
    EQ_CHECK(value(draw + stats.vertexShaderInvocations) >= 3);
    EQ_CHECK(value(draw + stats.inputPrimitives) == 1);
    // How pixel-shader invocations are counted is up to the GPU: AMD counts
    // 2x2 quads, so 1024 covered pixels may read 256.
    EQ_CHECK(value(draw + stats.pixelShaderInvocations) >= kSize * kSize / 4);
    EQ_CHECK(value(draw + stats.computeShaderInvocations) == 0);
    EQ_CHECK(value(dispatch + stats.computeShaderInvocations) == kSize * kSize);
    EQ_CHECK(value(dispatch + stats.pixelShaderInvocations) == 0);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyQueryPool(f.device, occlusion);
    RI::destroyQueryPool(f.device, statistics);
    RI::destroyBuffer(f.device, results);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
}

#if EQUINE_VALIDATION
// A copy needs the texture's copy usage (P47).
EQ_TEST(texture, copy_without_usage_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                        .usage = RI::TextureUsage::eSampled});
    RI::Buffer back = placeBuffer(f.device, f.readback, {.size = 1 << 16, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdCopyTextureToBuffer(f.cmd, texture.view, back, 0);
}
#endif

// Indexed draws (P39) with additive blending: two instances of the same
// full-screen triangle, each adding 0.25, give 0.5.
EQ_TEST(texture, indexed_draw_with_blending) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)}; // D3D12: 64 KiB per buffer
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 values[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, values, sizeof(values));

    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const RI::BlendState additive{.enable = true,
                                  .srcColor = RI::BlendFactor::eOne,
                                  .dstColor = RI::BlendFactor::eOne,
                                  .srcAlpha = RI::BlendFactor::eOne,
                                  .dstAlpha = RI::BlendFactor::eOne};
    RI::Pipeline pipeline = RI::createGraphicsPipeline(
        f.device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA16Float}, .blend = {additive}});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.25f, 0.25f, 0.25f}, .depth = 0.5f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdDrawIndexed(f.cmd, 3, 2);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 3, 0.5f));
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyTexture(f.device, target);
}

// The stencil test (P38): pass 1 writes 1 everywhere; a draw testing for 2 is
// stopped, one testing for 1 (writing only green) goes through.
EQ_TEST(texture, stencil_test) {
    Fixture f;
    const RI::Format depthFormat = RI::Format::eD32FloatS8Uint;
    if (!RI::formatSupported(f.device, depthFormat, RI::TextureUsage::eDepthStencil)) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize, .height = kSize, .format = depthFormat,
                                      .usage = RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const auto pipelineWith = [&](RI::CompareOp compare, RI::StencilOp pass, uint8 reference, RI::ColorMask mask) {
        const RI::StencilFace face{.pass = pass, .compare = compare};
        return RI::createGraphicsPipeline(
            f.device, {.vertex = vs,
                       .pixel = ps,
                       .colorFormats = {RI::Format::eRGBA8Unorm},
                       .depthFormat = depthFormat,
                       .stencil = {.test = true, .reference = reference, .front = face, .back = face},
                       .blend = {{.writeMask = mask}}});
    };
    RI::Pipeline write = pipelineWith(RI::CompareOp::eAlways, RI::StencilOp::eReplace, 1, RI::ColorMask::eNone);
    RI::Pipeline red = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 2, RI::ColorMask::eAll);
    RI::Pipeline green = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 1, RI::ColorMask::eG);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}},
                                  .depth = RI::DepthAttachment{.view = depth.view,
                                                               .load = RI::LoadOp::eClear,
                                                               .stencilLoad = RI::LoadOp::eClear}});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 0.0f, 0.0f, 0.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, red);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, green);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.0f) && all(texels, 1, 1.0f) && all(texels, 3, 0.0f));
    RI::destroyPipeline(f.device, write);
    RI::destroyPipeline(f.device, red);
    RI::destroyPipeline(f.device, green);
    RI::destroyTexture(f.device, depth);
    RI::destroyTexture(f.device, target);
}

// cmdSetStencilReference (P104): the override writes 3 and still holds after a
// pipeline switch (green passes testing for 3, its pipeline says 9); the next
// section is back to the pipeline's 9, which fails (red stays 0).
EQ_TEST(texture, stencil_reference_command) {
    Fixture f;
    const RI::Format depthFormat = RI::Format::eD32FloatS8Uint;
    if (!RI::formatSupported(f.device, depthFormat, RI::TextureUsage::eDepthStencil)) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize, .height = kSize, .format = depthFormat,
                                      .usage = RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const auto pipelineWith = [&](RI::CompareOp compare, RI::StencilOp pass, uint8 reference, RI::ColorMask mask) {
        const RI::StencilFace face{.pass = pass, .compare = compare};
        return RI::createGraphicsPipeline(
            f.device, {.vertex = vs,
                       .pixel = ps,
                       .colorFormats = {RI::Format::eRGBA8Unorm},
                       .depthFormat = depthFormat,
                       .stencil = {.test = true, .reference = reference, .front = face, .back = face},
                       .blend = {{.writeMask = mask}}});
    };
    RI::Pipeline write = pipelineWith(RI::CompareOp::eAlways, RI::StencilOp::eReplace, 0, RI::ColorMask::eNone);
    RI::Pipeline green = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 9, RI::ColorMask::eG);
    RI::Pipeline red = pipelineWith(RI::CompareOp::eEqual, RI::StencilOp::eKeep, 9, RI::ColorMask::eR);
    const DrawRoot white{.color = {1.0f, 1.0f, 1.0f, 1.0f}, .depth = 0.5f};
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}},
                                  .depth = RI::DepthAttachment{.view = depth.view,
                                                               .load = RI::LoadOp::eClear,
                                                               .stencilLoad = RI::LoadOp::eClear}});
    RI::cmdSetStencilReference(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetPipeline(f.cmd, green);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eLoad}},
                                  .depth = RI::DepthAttachment{.view = depth.view,
                                                               .load = RI::LoadOp::eLoad,
                                                               .stencilLoad = RI::LoadOp::eLoad}});
    RI::cmdSetPipeline(f.cmd, red);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.0f) && all(texels, 1, 1.0f));
    for (RI::Pipeline *p : {&write, &green, &red}) {
        RI::destroyPipeline(f.device, *p);
    }
    RI::destroyTexture(f.device, depth);
    RI::destroyTexture(f.device, target);
}

// Mesh pipelines (P39): a mesh shader alone, then a task shader launching it,
// each drawing the full-screen triangle.
EQ_TEST(texture, mesh_shaders) {
    Fixture f;
    if (!f.device.limits.meshShaders) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    // D3D12 requires a mesh shader's input payload to match its task shader's output.
    std::vector<std::byte> as = loadShader("fullscreen_as"), ms = loadShader("fullscreen_ms"),
                           payloadMs = loadShader("fullscreen_payload_ms"), ps = loadShader("solid_ps");
    RI::Pipeline meshOnly = RI::createGraphicsPipeline(
        f.device, {.pixel = ps, .mesh = ms, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::Pipeline taskMesh = RI::createGraphicsPipeline(
        f.device, {.pixel = ps, .task = as, .mesh = payloadMs, .colorFormats = {RI::Format::eRGBA8Unorm},
                   .blend = {{.writeMask = RI::ColorMask::eG}}});
    EQ_CHECK(meshOnly.rootDataSize == sizeof(DrawRoot) && taskMesh.rootDataSize == sizeof(DrawRoot));
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, meshOnly);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDrawMesh(f.cmd, 1, 1, 1);
    RI::cmdSetPipeline(f.cmd, taskMesh);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDrawMesh(f.cmd, 1, 1, 1);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 1.0f) && all(texels, 1, 1.0f) && all(texels, 3, 1.0f));
    RI::destroyPipeline(f.device, meshOnly);
    RI::destroyPipeline(f.device, taskMesh);
    RI::destroyTexture(f.device, target);
}

// The mesh counter set and the mesh-primitives query (P105) around a mesh draw
// of one full-screen triangle. Vulkan forbids the classic set there.
EQ_TEST(texture, mesh_statistics) {
    Fixture f;
    if (!f.device.limits.meshShaders) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> ms = loadShader("fullscreen_ms"), ps = loadShader("solid_ps");
    RI::Pipeline pipeline =
        RI::createGraphicsPipeline(f.device, {.pixel = ps, .mesh = ms, .colorFormats = {RI::Format::eRGBA8Unorm}});
    const RI::PipelineStatisticsLayout meshLayout =
        RI::pipelineStatisticsLayout(f.device, RI::QueryType::eMeshPipelineStatistics);
    const RI::PipelineStatisticsLayout primitivesLayout =
        RI::pipelineStatisticsLayout(f.device, RI::QueryType::eMeshPrimitives);
    EQ_CHECK(meshLayout.pixelShaderInvocations != RI::kNoCounter && meshLayout.computeShaderInvocations != RI::kNoCounter);
    EQ_CHECK(meshLayout.vertexShaderInvocations == RI::kNoCounter && meshLayout.clippingPrimitives == RI::kNoCounter &&
             meshLayout.meshPrimitives == RI::kNoCounter);
    EQ_CHECK(primitivesLayout.pixelShaderInvocations == RI::kNoCounter);
    const bool primitives = primitivesLayout.meshPrimitives != RI::kNoCounter;

    RI::QueryPool stats = RI::createQueryPool(f.device, {.type = RI::QueryType::eMeshPipelineStatistics, .count = 1});
    RI::QueryPool prims{};
    if (primitives) {
        prims = RI::createQueryPool(f.device, {.type = RI::QueryType::eMeshPrimitives, .count = 1});
    }
    RI::Buffer results = placeBuffer(f.device, f.readback,
                                     {.size = meshLayout.size + primitivesLayout.size, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdBeginQuery(f.cmd, stats, 0);
    if (primitives) {
        RI::cmdBeginQuery(f.cmd, prims, 0);
    }
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDrawMesh(f.cmd, 1, 1, 1);
    if (primitives) {
        RI::cmdEndQuery(f.cmd, prims, 0);
    }
    RI::cmdEndQuery(f.cmd, stats, 0);
    RI::cmdEndRendering(f.cmd);
    RI::cmdResolveQueries(f.cmd, stats, 0, 1, results, 0);
    if (primitives) {
        RI::cmdResolveQueries(f.cmd, prims, 0, 1, results, meshLayout.size);
    }
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 1.0f));
    const RI::Helpers::PipelineStatistics counts = RI::Helpers::readPipelineStatistics(meshLayout, results.mapped);
    // AMD counts 2x2 quads (see occlusion_and_statistics).
    EQ_CHECK(counts.pixelShaderInvocations >= kSize * kSize / 4 && counts.computeShaderInvocations == 0);
    EQ_CHECK(counts.vertexShaderInvocations == 0 && counts.meshPrimitives == 0);
    if (primitives) {
        const RI::Helpers::PipelineStatistics generated = RI::Helpers::readPipelineStatistics(
            primitivesLayout, static_cast<const std::byte *>(results.mapped) + meshLayout.size);
        EQ_CHECK(generated.meshPrimitives == 1);
        RI::destroyQueryPool(f.device, prims);
    }
    RI::destroyBuffer(f.device, results);
    RI::destroyQueryPool(f.device, stats);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
}

// Color resolves (P52): at cmdEndRendering through resolveTo, and with
// cmdResolve outside a section.
EQ_TEST(texture, resolve_color) {
    Fixture f;
    const RI::Format format = RI::Format::eRGBA16Float;
    if (!RI::formatSupported(f.device, format, RI::TextureUsage::eRenderTarget, 4)) {
        return;
    }
    RI::Texture msaa = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4,
                                                      .format = format, .usage = RI::TextureUsage::eRenderTarget});
    RI::Texture resolved = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format,
                                                          .usage = RI::TextureUsage::eRenderTarget |
                                                                   RI::TextureUsage::eSampled});
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline = drawPipeline(f.device, format, RI::Format::eUndefined, 4, false, vs, ps);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = msaa.view, .load = RI::LoadOp::eClear,
                                              .resolveTo = resolved.view}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.5f, 0.5f, 0.5f, 0.5f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.5f) && all(texels, 3, 0.5f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = msaa.view, .load = RI::LoadOp::eClear,
                                              .clear = {.color = {0.25f, 0.25f, 0.25f, 0.25f}}}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eRenderTarget);
    RI::cmdResolve(f.cmd, msaa.view, resolved.view, RI::ResolveMode::eAverage);
    texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 3, 0.25f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, resolved);
    RI::destroyTexture(f.device, msaa);
}

// Depth resolves (P76) with a mode the device reports, at cmdEndRendering and
// with cmdResolve.
EQ_TEST(texture, resolve_depth) {
    Fixture f;
    const RI::Format format = RI::Format::eD32Float;
    RI::ResolveMode mode = RI::ResolveMode::eMin;
    if ((f.device.limits.depthResolveModes & RI::resolveModeBit(mode)) == 0) {
        mode = RI::ResolveMode::eMax;
    }
    if ((f.device.limits.depthResolveModes & RI::resolveModeBit(mode)) == 0 ||
        !RI::formatSupported(f.device, format, RI::TextureUsage::eDepthStencil, 4)) {
        return;
    }
    RI::Texture msaa = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .samples = 4,
                                                      .format = format, .usage = RI::TextureUsage::eDepthStencil});
    RI::Texture resolved = placeTexture(f.device, f.gpu, {.width = kSize, .height = kSize, .format = format,
                                                          .usage = RI::TextureUsage::eDepthStencil |
                                                                   RI::TextureUsage::eSampled});
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = msaa.view,
                                                                .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.75f},
                                                                .resolveTo = resolved.view,
                                                                .resolveMode = mode}});
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.75f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::cmdBeginRendering(f.cmd, {.depth = RI::DepthAttachment{.view = msaa.view, .load = RI::LoadOp::eClear,
                                                                .clear = {.depth = 0.25f}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eDepthStencil, RI::Stage::eDepthStencil);
    RI::cmdResolve(f.cmd, msaa.view, resolved.view, mode);
    texels = f.readBack(resolved.view);
    EQ_CHECK(all(texels, 0, 0.25f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyTexture(f.device, resolved);
    RI::destroyTexture(f.device, msaa);
}

namespace {

// Additive draws of the full-screen triangle into a 16-bit float target.
struct AdditiveTarget {
    RI::Texture target;
    std::vector<std::byte> vs, ps;
    RI::Pipeline pipeline;

    explicit AdditiveTarget(Fixture &f) {
        target = placeTexture(f.device, f.gpu,
                              {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                               .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
        vs = loadShader("fullscreen_vs");
        ps = loadShader("solid_ps");
        const RI::BlendState additive{.enable = true,
                                      .srcColor = RI::BlendFactor::eOne,
                                      .dstColor = RI::BlendFactor::eOne,
                                      .srcAlpha = RI::BlendFactor::eOne,
                                      .dstAlpha = RI::BlendFactor::eOne};
        pipeline = RI::createGraphicsPipeline(
            f.device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA16Float}, .blend = {additive}});
    }
    void destroy(Fixture &f) {
        RI::destroyPipeline(f.device, pipeline);
        RI::destroyTexture(f.device, target);
    }
};

} // namespace

// Indirect Level 1 (P48): indexed draws cut short by a GPU count, plain draws,
// and a dispatch, all from buffer arguments.
EQ_TEST(texture, indirect_level1) {
    Fixture f;
    AdditiveTarget t(f);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)}; // D3D12: 64 KiB per buffer
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 indexValues[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, indexValues, sizeof(indexValues));
    RI::Buffer args = placeBuffer(f.device, upload, {.size = 256, .usage = RI::BufferUsage::eIndirect});
    auto *bytes = static_cast<std::byte *>(args.mapped);
    const RI::DrawIndexedIndirectArgs indexed{.indexCount = 3, .instanceCount = 1};
    std::memcpy(bytes, &indexed, sizeof(indexed));                  // record 0
    std::memcpy(bytes + sizeof(indexed), &indexed, sizeof(indexed)); // record 1, cut by the count
    const RI::DrawIndirectArgs plain{.vertexCount = 3, .instanceCount = 1};
    std::memcpy(bytes + 64, &plain, sizeof(plain));
    std::memcpy(bytes + 64 + sizeof(plain), &plain, sizeof(plain));
    const RI::DispatchIndirectArgs groups{kSize / 8, kSize / 8, 1};
    std::memcpy(bytes + 128, &groups, sizeof(groups));
    RI::Buffer count = placeBuffer(f.device, upload, {.size = 4, .usage = RI::BufferUsage::eIndirect});
    *static_cast<uint32 *>(count.mapped) = 1;

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = t.target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, t.pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.25f, 0.25f, 0.25f, 0.25f}, .depth = 0.5f});
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDrawIndexed, args, 0, 2, &count, 0); // one draw: +0.25
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDraw, args, 64, 2);                  // two draws: +0.5
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(t.target.view);
    EQ_CHECK(all(texels, 0, 0.75f));

    // Dispatch: the storage-write shader fills a texture.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = storage.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, WriteRoot{.textureSlot = 2, .width = kSize, .height = kSize, .value = 3.0f});
    RI::cmdExecuteIndirect(f.cmd, RI::IndirectKind::eDispatch, args, 128, 1);
    texels = f.readBack(storage.view);
    EQ_CHECK(all(texels, 2, 3.0f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, storage);
    RI::destroyBuffer(f.device, count);
    RI::destroyBuffer(f.device, args);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    t.destroy(f);
}

// Indirect Level 2 (P48): each record sets the root data, then draws or
// dispatches; the GPU count stops before the third record.
EQ_TEST(texture, indirect_level2) {
    Fixture f;
    if (!f.device.limits.indirectRootData) {
        return;
    }
    AdditiveTarget t(f);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)}; // D3D12: 64 KiB per buffer
    RI::Buffer indices = placeBuffer(f.device, upload, {.size = 16, .usage = RI::BufferUsage::eIndex});
    const uint16 indexValues[] = {0, 1, 2, 0};
    std::memcpy(indices.mapped, indexValues, sizeof(indexValues));
    struct DrawRecord {
        DrawRoot root;
        RI::DrawIndexedIndirectArgs args;
    };
    static_assert(sizeof(DrawRecord) == RI::indirectStride(RI::IndirectKind::eDrawIndexed, sizeof(DrawRoot)));
    RI::Buffer records = placeBuffer(f.device, upload, {.size = 3 * sizeof(DrawRecord), .usage = RI::BufferUsage::eIndirect});
    const RI::DrawIndexedIndirectArgs draw{.indexCount = 3, .instanceCount = 1};
    const DrawRecord list[3] = {{{.color = {0.25f, 0.0f, 0.0f, 0.0f}, .depth = 0.5f}, draw},
                                {{.color = {0.0f, 0.5f, 0.0f, 0.0f}, .depth = 0.5f}, draw},
                                {{.color = {0.0f, 0.0f, 1.0f, 0.0f}, .depth = 0.5f}, draw}};
    std::memcpy(records.mapped, list, sizeof(list));
    RI::Buffer count = placeBuffer(f.device, upload, {.size = 4, .usage = RI::BufferUsage::eIndirect});
    *static_cast<uint32 *>(count.mapped) = 2;

    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = t.target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, t.pipeline);
    RI::cmdSetIndexBuffer(f.cmd, indices, 0, RI::IndexFormat::eUint16);
    // The scratch the call needs (none on D3D12), in memory of a kind it allows.
    std::vector<RI::Memory> scratchMemory;
    const auto makeScratch = [&](const RI::Pipeline &pipeline, RI::IndirectKind kind, uint32 maxCount) {
        const RI::MemoryRequirements need = RI::indirectScratchRequirements(f.device, pipeline, kind, maxCount);
        if (need.size == 0) {
            return RI::Buffer{};
        }
        EQ_CHECK(RI::fits(need, RI::MemoryKind::eGPU));
        // Memory of a type the scratch allows (RADV: only some GPU types).
        scratchMemory.push_back(RI::allocateMemory(f.device, RI::MemoryKind::eGPU, need.size, &need));
        return RI::createBuffer(f.device, scratchMemory.back(), 0,
                                {.size = need.size, .usage = RI::BufferUsage::eIndirectScratch});
    };
    RI::Buffer drawScratch = makeScratch(t.pipeline, RI::IndirectKind::eDrawIndexed, 3);
    RI::cmdExecuteIndirectWithRootData(f.cmd, RI::IndirectKind::eDrawIndexed, records, 0, 3,
                                       drawScratch.size > 0 ? &drawScratch : nullptr, 0, &count, 0);
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(t.target.view);
    EQ_CHECK(all(texels, 0, 0.25f) && all(texels, 1, 0.5f) && all(texels, 2, 0.0f));

    // A dispatch whose root data comes from the record.
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::Texture storage = placeTexture(f.device, f.gpu,
                                       {.width = kSize, .height = kSize, .format = RI::Format::eRGBA16Float,
                                        .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled});
    RI::writeDescriptors(f.device, f.heap, {{.slot = 2, .descriptor = RI::ReadWriteView{.texture = storage.view}}});
    const std::vector<std::byte> shader = loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    struct DispatchRecord {
        WriteRoot root;
        RI::DispatchIndirectArgs args;
    };
    RI::Buffer dispatches = placeBuffer(f.device, upload, {.size = sizeof(DispatchRecord), .usage = RI::BufferUsage::eIndirect});
    const DispatchRecord record{{.textureSlot = 2, .width = kSize, .height = kSize, .value = 5.0f}, {kSize / 8, kSize / 8, 1}};
    std::memcpy(dispatches.mapped, &record, sizeof(record));
    RI::cmdSetPipeline(f.cmd, write);
    RI::Buffer dispatchScratch = makeScratch(write, RI::IndirectKind::eDispatch, 1);
    RI::cmdExecuteIndirectWithRootData(f.cmd, RI::IndirectKind::eDispatch, dispatches, 0, 1,
                                       dispatchScratch.size > 0 ? &dispatchScratch : nullptr, 0);
    texels = f.readBack(storage.view);
    EQ_CHECK(all(texels, 2, 5.0f));

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, write);
    RI::destroyTexture(f.device, storage);
    RI::destroyBuffer(f.device, dispatches);
    for (RI::Buffer *scratch : {&drawScratch, &dispatchScratch}) {
        if (scratch->size > 0) {
            RI::destroyBuffer(f.device, *scratch);
        }
    }
    for (RI::Memory &memory : scratchMemory) {
        RI::freeMemory(f.device, memory);
    }
    RI::destroyBuffer(f.device, count);
    RI::destroyBuffer(f.device, records);
    RI::destroyBuffer(f.device, indices);
    RI::freeMemory(f.device, upload.memory);
    t.destroy(f);
}

// Samplers (P87): a 2x2 texture whose red channel is 0 in column 0 and 1 in
// column 1, sampled through filters, address modes, a border and min/max
// reductions written with writeSamplers.
EQ_TEST(texture, samplers) {
    Fixture f;
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)}; // D3D12: 64 KiB per buffer
    RI::Texture texture = placeTexture(f.device, f.gpu,
                                       {.width = 2, .height = 2, .format = RI::Format::eRGBA8Unorm,
                                        .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopyDst});
    const RI::CopyLayout layout = RI::copyLayout(f.device, texture.view);
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    for (uint32 y = 0; y < 2; ++y) {
        const uint8 row[8] = {0, 0, 0, 255, 255, 0, 0, 255};
        std::memcpy(static_cast<std::byte *>(staging.mapped) + y * layout.rowPitch, row, sizeof(row));
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, texture.view);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCompute);

    using RI::AddressMode;
    using RI::Filter;
    std::vector<RI::SamplerWrite> samplers = {
        {.slot = 0, .sampler = {.minFilter = Filter::eNearest, .magFilter = Filter::eNearest,
                                .mipFilter = Filter::eNearest, .addressU = AddressMode::eClamp,
                                .addressV = AddressMode::eClamp}},
        {.slot = 1, .sampler = {.addressU = AddressMode::eClamp, .addressV = AddressMode::eClamp}},
        {.slot = 2, .sampler = {}}, // linear, repeat
        {.slot = 3, .sampler = {.addressU = AddressMode::eBorder, .addressV = AddressMode::eBorder,
                                .border = RI::BorderColor::eOpaqueWhite}},
    };
    if (f.device.limits.samplerMinMax) {
        samplers.push_back({.slot = 4, .sampler = {.addressU = AddressMode::eClamp, .addressV = AddressMode::eClamp,
                                                   .reduction = RI::Reduction::eMin}});
        samplers.push_back({.slot = 5, .sampler = {.addressU = AddressMode::eClamp, .addressV = AddressMode::eClamp,
                                                   .reduction = RI::Reduction::eMax}});
    }
    RI::DescriptorHeap heap = RI::createDescriptorHeap(f.device, {.resourceCount = 2, .samplerCount = 8});
    RI::writeSamplers(f.device, heap, samplers);
    // Written, not sampled: a comparison sampler, and a repeat of slot 1's
    // description (one native sampler on Vulkan).
    RI::writeSamplers(f.device, heap, {{.slot = 6, .sampler = {.compare = RI::CompareOp::eLess}},
                                       {.slot = 7, .sampler = samplers[1].sampler}});
    RI::writeDescriptors(f.device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = f.output, .stride = 16}},
                                          {.slot = 1, .descriptor = RI::ReadView{.texture = texture.view}}});
    const std::vector<std::byte> shader = loadShader("sample_texture");
    RI::Pipeline pipeline = RI::createComputePipeline(f.device, {.shader = shader});
    struct SampleRoot {
        uint32 textureSlot, outputSlot, count;
    };
    RI::cmdSetHeap(f.cmd, heap);
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, SampleRoot{.textureSlot = 1, .outputSlot = 0, .count = uint32(samplers.size())});
    RI::cmdDispatch(f.cmd, 1, 1, 1);
    RI::cmdBarrier(f.cmd, RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, samplers.size() * 16);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    // Per sampler: red at (0.5, 0.5), (0, 0), (0.75, 0.25), (-0.5, -0.5).
    const auto *r = static_cast<const float *>(f.result.mapped);
    EQ_CHECK(near(r[0 * 4 + 2], 1.0f) && near(r[0 * 4 + 3], 0.0f)); // nearest, clamp
    EQ_CHECK(near(r[1 * 4 + 0], 0.5f) && near(r[1 * 4 + 1], 0.0f) && near(r[1 * 4 + 3], 0.0f)); // linear, clamp
    EQ_CHECK(near(r[2 * 4 + 0], 0.5f) && near(r[2 * 4 + 1], 0.5f)); // linear, repeat: column 1 wraps in
    EQ_CHECK(near(r[3 * 4 + 3], 1.0f));                              // border, opaque white
    if (f.device.limits.samplerMinMax) {
        EQ_CHECK(near(r[4 * 4 + 0], 0.0f) && near(r[5 * 4 + 0], 1.0f)); // min and max of the four texels
    }

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyDescriptorHeap(f.device, heap);
    RI::destroyTexture(f.device, texture);
    RI::destroyBuffer(f.device, staging);
    RI::freeMemory(f.device, upload.memory);
}

// A pixel shader that takes the vertex shader's output struct as written links
// on both backends (P101).
EQ_TEST(texture, stage_link) {
    RI::Device device = createTestDevice();
    const std::vector<std::byte> vs = loadShader("link_vs"), ps = loadShader("link_ps");
    RI::Pipeline pipeline =
        RI::createGraphicsPipeline(device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA8Unorm}});
    RI::destroyPipeline(device, pipeline);
    RI::destroyDevice(device);
}

namespace {

// Pixels whose red channel is set.
uint32 lit(const std::vector<float> &texels) {
    uint32 count = 0;
    for (size_t i = 0; i < texels.size(); i += 4) {
        count += texels[i] > 0.5f ? 1u : 0u;
    }
    return count;
}

// The texel at (x, y) of a kSize x kSize readback, channel `channel`.
float texel(const std::vector<float> &texels, uint32 x, uint32 y, uint32 channel) {
    return texels[(y * kSize + x) * 4 + channel];
}

// Draws `count` vertices of `vs` with `desc`'s other state into a cleared
// render target and reads it back.
std::vector<float> drawAndRead(Fixture &f, RI::GraphicsPipelineDesc desc, uint32 count, float depth) {
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    desc.colorFormats = {RI::Format::eRGBA8Unorm};
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, desc);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 0.0f, 0.0f, 1.0f}, .depth = depth});
    RI::cmdDraw(f.cmd, count);
    RI::cmdEndRendering(f.cmd);
    std::vector<float> texels = f.readBack(target.view);
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
    return texels;
}

} // namespace

// Line strips (P102): three points draw two segments where a line list of the
// same points draws one.
EQ_TEST(texture, line_strip) {
    Fixture f;
    std::vector<std::byte> vs = loadShader("lines_vs"), ps = loadShader("solid_ps");
    const std::vector<float> list = drawAndRead(f, {.vertex = vs, .pixel = ps, .topology = RI::Topology::eLineList}, 2, 0.5f);
    const std::vector<float> strip = drawAndRead(f, {.vertex = vs, .pixel = ps, .topology = RI::Topology::eLineStrip}, 3, 0.5f);
    EQ_CHECK(lit(list) >= kSize / 2 && lit(list) <= kSize + 2);
    EQ_CHECK(lit(strip) > lit(list) + kSize / 4);
}

// Wireframe (P102): the triangle's edges are drawn, its inside is not.
EQ_TEST(texture, wireframe) {
    Fixture f;
    std::vector<std::byte> vs = loadShader("triangle_vs"), ps = loadShader("solid_ps");
    const std::vector<float> solid = drawAndRead(f, {.vertex = vs, .pixel = ps}, 3, 0.5f);
    const std::vector<float> wire = drawAndRead(f, {.vertex = vs, .pixel = ps, .fill = RI::FillMode::eWireframe}, 3, 0.5f);
    EQ_CHECK(texel(solid, kSize / 2, kSize / 2, 0) > 0.5f);
    EQ_CHECK(texel(wire, kSize / 2, kSize / 2, 0) < 0.5f);
    EQ_CHECK(lit(wire) > kSize && lit(wire) < lit(solid));
}

// Depth bias (P102): a second draw at the same depth fails eLess, unless
// biased towards the viewer.
EQ_TEST(texture, depth_bias) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    RI::Texture depth = placeTexture(f.device, f.gpu,
                                     {.width = kSize, .height = kSize, .format = RI::Format::eD32Float,
                                      .usage = RI::TextureUsage::eDepthStencil});
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const auto pipelineWith = [&](RI::DepthState state, RI::DepthBias bias, RI::ColorMask mask) {
        return RI::createGraphicsPipeline(f.device, {.vertex = vs,
                                                     .pixel = ps,
                                                     .colorFormats = {RI::Format::eRGBA8Unorm},
                                                     .depthFormat = RI::Format::eD32Float,
                                                     .depth = state,
                                                     .depthBias = bias,
                                                     .blend = {{.writeMask = mask}}});
    };
    RI::Pipeline base = pipelineWith({.test = true, .write = true, .compare = RI::CompareOp::eAlways}, {},
                                     RI::ColorMask::eNone);
    RI::Pipeline plain = pipelineWith({.test = true, .compare = RI::CompareOp::eLess}, {}, RI::ColorMask::eR);
    RI::Pipeline biased =
        pipelineWith({.test = true, .compare = RI::CompareOp::eLess}, {.constant = -100}, RI::ColorMask::eG);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}},
                                  .depth = RI::DepthAttachment{.view = depth.view, .load = RI::LoadOp::eClear}});
    for (RI::Pipeline *p : {&base, &plain, &biased}) {
        RI::cmdSetPipeline(f.cmd, *p);
        RI::cmdSetRootData(f.cmd, DrawRoot{.color = {1.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
        RI::cmdDraw(f.cmd, 3);
    }
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.0f) && all(texels, 1, 1.0f));
    for (RI::Pipeline *p : {&base, &plain, &biased}) {
        RI::destroyPipeline(f.device, *p);
    }
    RI::destroyTexture(f.device, depth);
    RI::destroyTexture(f.device, target);
}

// Depth clamp (P102): a triangle beyond the far plane is clipped, unless
// depth is clamped.
EQ_TEST(texture, depth_clamp) {
    Fixture f;
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const std::vector<float> clipped = drawAndRead(f, {.vertex = vs, .pixel = ps}, 3, 1.5f);
    const std::vector<float> clamped = drawAndRead(f, {.vertex = vs, .pixel = ps, .depthClamp = true}, 3, 1.5f);
    EQ_CHECK(all(clipped, 0, 0.0f));
    EQ_CHECK(all(clamped, 0, 1.0f));
}

// A geometry shader (P102) turns one point into the full-screen triangle; the
// pixel shader reads the geometry shader's outputs (P101), and the statistics
// count one geometry shader run.
EQ_TEST(texture, geometry_shader) {
    Fixture f;
    if (!f.device.limits.geometryShaders) {
        return;
    }
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> vs = loadShader("point_vs"), gs = loadShader("fullscreen_gs"), ps = loadShader("color_ps");
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, {.vertex = vs,
                                                                  .geometry = gs,
                                                                  .pixel = ps,
                                                                  .colorFormats = {RI::Format::eRGBA8Unorm},
                                                                  .topology = RI::Topology::ePointList});
    RI::QueryPool statistics = RI::createQueryPool(f.device, {.type = RI::QueryType::ePipelineStatistics, .count = 1});
    const RI::PipelineStatisticsLayout stats = RI::pipelineStatisticsLayout(f.device);
    EQ_CHECK(stats.geometryShaderInvocations != RI::kNoCounter && stats.geometryShaderPrimitives != RI::kNoCounter);
    RI::Buffer results = placeBuffer(f.device, f.readback, {.size = stats.size, .usage = RI::BufferUsage::eCopyDst});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdBeginQuery(f.cmd, statistics, 0);
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 1.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 1);
    RI::cmdEndQuery(f.cmd, statistics, 0);
    RI::cmdEndRendering(f.cmd);
    RI::cmdResolveQueries(f.cmd, statistics, 0, 1, results, 0);
    const std::vector<float> texels = f.readBack(target.view);
    EQ_CHECK(all(texels, 0, 0.0f) && all(texels, 1, 1.0f));
    const auto *base = static_cast<const uint8 *>(results.mapped);
    const auto value = [&](uint32 offset) { return *reinterpret_cast<const uint64 *>(base + offset); };
    EQ_CHECK(value(stats.geometryShaderInvocations) == 1);
    EQ_CHECK(value(stats.geometryShaderPrimitives) == 1);
    // The same counters through the helper's fixed struct.
    const RI::Helpers::PipelineStatistics read = RI::Helpers::readPipelineStatistics(stats, results.mapped);
    EQ_CHECK(read.geometryShaderInvocations == 1 && read.geometryShaderPrimitives == 1);
    EQ_CHECK(read.inputPrimitives == 1 && read.computeShaderInvocations == 0);
    RI::destroyBuffer(f.device, results);
    RI::destroyQueryPool(f.device, statistics);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyTexture(f.device, target);
}

// Tessellation (P102): one three-point patch with factors of 1 becomes the
// full-screen triangle; the pixel shader reads the domain shader's outputs.
EQ_TEST(texture, tessellation) {
    Fixture f;
    if (!f.device.limits.tessellation) {
        return;
    }
    std::vector<std::byte> vs = loadShader("patch_vs"), hs = loadShader("patch_hs"), ds = loadShader("patch_ds"),
                           ps = loadShader("color_ps");
    const std::vector<float> texels = drawAndRead(f,
                                                  {.vertex = vs,
                                                   .hull = hs,
                                                   .domain = ds,
                                                   .pixel = ps,
                                                   .topology = RI::Topology::ePatchList,
                                                   .patchControlPoints = 3},
                                                  3, 0.5f);
    EQ_CHECK(all(texels, 0, 1.0f) && all(texels, 3, 1.0f));
}

namespace {

struct SampleCubeRoot {
    uint32 cubeSlot, samplerSlot, outputSlot;
};

// Samples `cube` along the six axes with a heap of its own (slot 2, sampler 0),
// after `record` has written it, and checks face f holds the color the
// write_faces and layered_gs shaders give it.
void checkCubeFaces(Fixture &f, RI::DescriptorHeap &heap, const RI::Texture &cube) {
    RI::writeSamplers(f.device, heap,
                      {{.slot = 0, .sampler = {.minFilter = RI::Filter::eNearest, .magFilter = RI::Filter::eNearest,
                                               .mipFilter = RI::Filter::eNearest}}});
    RI::writeDescriptors(f.device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = f.output, .stride = 16}},
                                          {.slot = 2, .descriptor = RI::ReadView{.texture = cube.view}}});
    const std::vector<std::byte> shader = loadShader("sample_cube");
    RI::Pipeline sample = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, sample);
    RI::cmdSetRootData(f.cmd, SampleCubeRoot{.cubeSlot = 2, .samplerSlot = 0, .outputSlot = 0});
    RI::cmdDispatch(f.cmd, 1, 1, 1);
    RI::cmdBarrier(f.cmd, RI::Stage::eCompute, RI::Stage::eCopy);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, 6 * 16);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *r = static_cast<const float *>(f.result.mapped);
    for (uint32 face = 0; face < 6; ++face) {
        EQ_CHECK(near(r[face * 4 + 0], face / 8.0f + 1.0f / 16.0f) && near(r[face * 4 + 1], 1.0f - face / 8.0f));
    }
    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyPipeline(f.device, sample);
}

} // namespace

// A cube texture (P102): a compute shader writes its faces as a 2D array,
// then reads them back as a cube along each axis, in +X, -X, +Y, -Y, +Z, -Z
// order on both backends.
EQ_TEST(texture, cube_storage_then_sample) {
    Fixture f;
    RI::Texture cube = placeTexture(f.device, f.gpu,
                                    {.shape = RI::TextureShape::eCube, .width = 16, .height = 16, .layers = 6,
                                     .format = RI::Format::eRGBA8Unorm,
                                     .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(f.device, {.resourceCount = 4, .samplerCount = 1});
    RI::writeDescriptors(f.device, heap, {{.slot = 1, .descriptor = RI::ReadWriteView{.texture = cube.view}}});
    const std::vector<std::byte> shader = loadShader("write_faces");
    RI::Pipeline write = RI::createComputePipeline(f.device, {.shader = shader});
    struct WriteFacesRoot {
        uint32 textureSlot, size;
    };
    RI::cmdSetHeap(f.cmd, heap);
    RI::cmdSetPipeline(f.cmd, write);
    RI::cmdSetRootData(f.cmd, WriteFacesRoot{.textureSlot = 1, .size = 16});
    RI::cmdDispatch(f.cmd, 2, 2, 6);
    RI::cmdBarrier(f.cmd, RI::Stage::eCompute, RI::Stage::eCompute);
    checkCubeFaces(f, heap, cube);
    RI::destroyPipeline(f.device, write);
    RI::destroyDescriptorHeap(f.device, heap);
    RI::destroyTexture(f.device, cube);
}

// Rendering into all six faces of a cube in one draw (P102): a geometry shader
// picks the layer of each triangle (SV_RenderTargetArrayIndex).
EQ_TEST(texture, cube_layered_render) {
    Fixture f;
    if (!f.device.limits.geometryShaders) {
        return;
    }
    RI::Texture cube = placeTexture(f.device, f.gpu,
                                    {.shape = RI::TextureShape::eCube, .width = 16, .height = 16, .layers = 6,
                                     .format = RI::Format::eRGBA8Unorm,
                                     .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> vs = loadShader("point_vs"), gs = loadShader("layered_gs"), ps = loadShader("color_ps");
    RI::Pipeline pipeline = RI::createGraphicsPipeline(f.device, {.vertex = vs,
                                                                  .geometry = gs,
                                                                  .pixel = ps,
                                                                  .colorFormats = {RI::Format::eRGBA8Unorm},
                                                                  .topology = RI::Topology::ePointList});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(f.device, {.resourceCount = 4, .samplerCount = 1});
    RI::cmdSetHeap(f.cmd, heap);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = cube.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetPipeline(f.cmd, pipeline);
    RI::cmdSetRootData(f.cmd, DrawRoot{.color = {0.0f, 0.0f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(f.cmd, 1);
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eCompute);
    checkCubeFaces(f, heap, cube);
    RI::destroyPipeline(f.device, pipeline);
    RI::destroyDescriptorHeap(f.device, heap);
    RI::destroyTexture(f.device, cube);
}

// A 3D texture (P102): uploaded slice by slice, read back in a shader as a
// volume, then one slice rendered to and the whole volume copied out.
EQ_TEST(texture, volume_copy_render_sample) {
    Fixture f;
    constexpr uint32 kSide = 8, kDepth = 4;
    RI::Texture volume = placeTexture(f.device, f.gpu,
                                      {.shape = RI::TextureShape::e3D, .width = kSide, .height = kSide,
                                       .depth = kDepth, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eRenderTarget |
                                                RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst});
    const RI::CopyLayout layout = RI::copyLayout(f.device, volume.view);
    EQ_CHECK(layout.layers == kDepth && layout.layerPitch == layout.rowPitch * kSide);
    Arena upload{.memory = RI::allocateMemory(f.device, RI::MemoryKind::eUpload, 1 << 20)};
    RI::Buffer staging = placeBuffer(f.device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
    RI::Buffer copied = placeBuffer(f.device, f.readback, {.size = layout.size, .usage = RI::BufferUsage::eCopyDst});
    const auto texelAt = [&](const void *base, uint32 x, uint32 y, uint32 z) {
        return static_cast<const uint8 *>(base) + z * layout.layerPitch + y * layout.rowPitch + x * 4;
    };
    for (uint32 z = 0; z < kDepth; ++z) {
        for (uint32 y = 0; y < kSide; ++y) {
            for (uint32 x = 0; x < kSide; ++x) {
                auto *t = const_cast<uint8 *>(texelAt(staging.mapped, x, y, z));
                t[0] = static_cast<uint8>(40 * z + 10);
                t[1] = static_cast<uint8>(16 * x);
                t[2] = static_cast<uint8>(16 * y);
                t[3] = 255;
            }
        }
    }
    RI::cmdCopyBufferToTexture(f.cmd, staging, 0, volume.view);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::eCompute);

    struct ReadVolumeRoot {
        uint32 textureSlot, outputSlot, size, depth;
    };
    RI::writeDescriptors(f.device, f.heap, {{.slot = kTextureSlot, .descriptor = RI::ReadView{.texture = volume.view}}});
    const std::vector<std::byte> shader = loadShader("read_volume");
    RI::Pipeline read = RI::createComputePipeline(f.device, {.shader = shader});
    RI::cmdSetPipeline(f.cmd, read);
    RI::cmdSetRootData(f.cmd, ReadVolumeRoot{.textureSlot = kTextureSlot, .outputSlot = kOutputSlot, .size = kSide,
                                             .depth = kDepth});
    RI::cmdDispatch(f.cmd, kSide / 4, kSide / 4, kDepth / 4);
    RI::cmdBarrier(f.cmd, RI::Stage::eCompute, RI::Stage::eCopy | RI::Stage::eRenderTarget);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, kSide * kSide * kDepth * 16);

    RI::TextureView slice = RI::createView(f.device, volume, {.mipCount = 1, .baseLayer = 2, .layerCount = 1});
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = slice, .load = RI::LoadOp::eClear,
                                              .clear = {.color = {1.0f, 0.0f, 1.0f, 1.0f}}}}});
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::eRenderTarget, RI::Stage::eCopy);
    RI::cmdCopyTextureToBuffer(f.cmd, volume.view, copied, 0);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));

    const auto *shaderTexels = static_cast<const float *>(f.result.mapped);
    bool shaderOk = true, copyOk = true;
    for (uint32 z = 0; z < kDepth; ++z) {
        for (uint32 y = 0; y < kSide; ++y) {
            for (uint32 x = 0; x < kSide; ++x) {
                const float *s = shaderTexels + ((z * kSide + y) * kSide + x) * 4;
                shaderOk = shaderOk && near(s[0], (40.0f * z + 10.0f) / 255.0f) && near(s[1], 16.0f * x / 255.0f) &&
                           near(s[2], 16.0f * y / 255.0f);
                const uint8 *c = texelAt(copied.mapped, x, y, z);
                copyOk = copyOk && (z == 2 ? c[0] == 255 && c[1] == 0 && c[2] == 255
                                           : c[0] == 40 * z + 10 && c[1] == 16 * x && c[2] == 16 * y);
            }
        }
    }
    EQ_CHECK(shaderOk);
    EQ_CHECK(copyOk);

    RI::resetCommandList(f.cmd);
    RI::cmdBegin(f.cmd);
    RI::cmdSetHeap(f.cmd, f.heap);
    RI::destroyView(f.device, slice);
    RI::destroyPipeline(f.device, read);
    RI::destroyBuffer(f.device, copied);
    RI::destroyBuffer(f.device, staging);
    RI::freeMemory(f.device, upload.memory);
    RI::destroyTexture(f.device, volume);
}

// Viewport and scissor (P103): a viewport over the left half draws red there
// only; a scissor over the top-right quarter keeps green there; the next
// section starts again with both covering the attachments (blue everywhere).
EQ_TEST(texture, viewport_and_scissor) {
    Fixture f;
    RI::Texture target = placeTexture(f.device, f.gpu,
                                      {.width = kSize, .height = kSize, .format = RI::Format::eRGBA8Unorm,
                                       .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled});
    std::vector<std::byte> vs = loadShader("fullscreen_vs"), ps = loadShader("solid_ps");
    const auto writing = [&](RI::ColorMask mask) {
        return RI::createGraphicsPipeline(f.device, {.vertex = vs,
                                                     .pixel = ps,
                                                     .colorFormats = {RI::Format::eRGBA8Unorm},
                                                     .blend = {{.writeMask = mask}}});
    };
    RI::Pipeline red = writing(RI::ColorMask::eR), green = writing(RI::ColorMask::eG),
                 blue = writing(RI::ColorMask::eB);
    const DrawRoot white{.color = {1.0f, 1.0f, 1.0f, 1.0f}, .depth = 0.5f};
    constexpr float kHalf = kSize / 2.0f;
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eClear}}});
    RI::cmdSetViewport(f.cmd, {.width = kHalf, .height = float(kSize)});
    RI::cmdSetPipeline(f.cmd, red);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetViewport(f.cmd, {.width = float(kSize), .height = float(kSize)});
    RI::cmdSetScissor(f.cmd, {.x = kSize / 2, .y = 0, .width = kSize / 2, .height = kSize / 2});
    RI::cmdSetPipeline(f.cmd, green);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    RI::cmdBeginRendering(f.cmd, {.colors = {{.view = target.view, .load = RI::LoadOp::eLoad}}});
    RI::cmdSetPipeline(f.cmd, blue);
    RI::cmdSetRootData(f.cmd, white);
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    const std::vector<float> texels = f.readBack(target.view);
    bool ok = true;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            const bool left = x < kSize / 2, topRight = x >= kSize / 2 && y < kSize / 2;
            ok = ok && near(texel(texels, x, y, 0), left ? 1.0f : 0.0f) &&
                 near(texel(texels, x, y, 1), topRight ? 1.0f : 0.0f) && near(texel(texels, x, y, 2), 1.0f);
        }
    }
    EQ_CHECK(ok);
    for (RI::Pipeline *p : {&red, &green, &blue}) {
        RI::destroyPipeline(f.device, *p);
    }
    RI::destroyTexture(f.device, target);
}

// A section without attachments (P105): pixel shaders write only storage, the
// size bounds rasterization, and the viewport starts at the whole size.
struct RasterRoot {
    uint32 bufferSlot;
    uint32 width;
    uint32 value;
};

EQ_TEST(texture, rendering_without_attachments) {
    Fixture f;
    constexpr uint32 kBufferSlot = 2;
    RI::writeDescriptors(f.device, f.heap,
                         {{.slot = kBufferSlot, .descriptor = RI::ReadWriteView{.buffer = f.output, .stride = 4}}});
    std::vector<std::byte> vs = loadShader("storage_raster_vs"), ps = loadShader("storage_raster_ps");
    RI::Pipeline raster = RI::createGraphicsPipeline(f.device, {.vertex = vs, .pixel = ps});
    RI::cmdClearBuffer(f.cmd, f.output, ~0u);
    RI::cmdBarrier(f.cmd, RI::Stage::eCopy, RI::Stage::ePixel);
    RI::cmdBeginRendering(f.cmd, RI::RenderingSize{.width = kSize, .height = kSize});
    RI::cmdSetPipeline(f.cmd, raster);
    RI::cmdSetRootData(f.cmd, RasterRoot{.bufferSlot = kBufferSlot, .width = kSize, .value = 7});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdSetViewport(f.cmd, {.width = kSize / 2.0f, .height = float(kSize)});
    RI::cmdSetRootData(f.cmd, RasterRoot{.bufferSlot = kBufferSlot, .width = kSize, .value = 3});
    RI::cmdDraw(f.cmd, 3);
    RI::cmdEndRendering(f.cmd);
    RI::cmdBarrier(f.cmd, RI::Stage::ePixel, RI::Stage::eCopy);
    RI::cmdCopyBuffer(f.cmd, f.output, 0, f.result, 0, kSize * kSize * 4);
    RI::cmdEnd(f.cmd);
    RI::wait(f.device.graphicsQueue, RI::submit(f.device.graphicsQueue, f.cmd));
    const auto *values = static_cast<const uint32 *>(f.result.mapped);
    uint32 wrong = 0;
    for (uint32 y = 0; y < kSize; ++y) {
        for (uint32 x = 0; x < kSize; ++x) {
            wrong += values[y * kSize + x] != (x < kSize / 2 ? 3u : 7u) ? 1 : 0;
        }
    }
    EQ_CHECK(wrong == 0);
    RI::destroyPipeline(f.device, raster);
}

// A RenderingDesc with no attachment points to the size form (P105), in every build.
EQ_TEST(texture, rendering_desc_without_attachment_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    Fixture f;
    RI::cmdBeginRendering(f.cmd, RI::RenderingDesc{});
}

#if EQUINE_VALIDATION
// A pixel shader that leaves out values before the one it reads (P101): RI
// reports it on both backends, where Vulkan alone would read the wrong value.
EQ_TEST(texture, stage_link_mismatch_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = createTestDevice();
    const std::vector<std::byte> vs = loadShader("link_vs"), ps = loadShader("link_skip_ps");
    RI::createGraphicsPipeline(device, {.vertex = vs, .pixel = ps, .colorFormats = {RI::Format::eRGBA8Unorm}});
}
#endif
