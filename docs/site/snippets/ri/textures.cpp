// RI: textures, views, rendering, clears, discard and timestamps, the
// Milestone 3 API in one program. The reference pages show its marked
// sections. Compiled once per enabled backend; the shaders are
// snippets/hlsl/doc_triangle_vs.hlsl and doc_triangle_ps.hlsl, compiled to
// EQUINE_SHADER_DIR. No image layout appears anywhere: they are RI's business.

#include <RI/RI.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef EQUINE_SHADER_DIR
#define EQUINE_SHADER_DIR "shaders"
#endif

namespace {

// RI takes bytecode only (P29): SPIR-V on Vulkan, DXIL on D3D12.
std::vector<std::byte> loadShader(const std::string &name) {
#if EQUINE_BACKEND_VULKAN
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".spv";
#else
    const std::string path = std::string(EQUINE_SHADER_DIR) + "/" + name + ".dxil";
#endif
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

uint64 alignUp(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }

// --8<-- [start:root-struct]
// The C++ mirror of `DrawRoot` in the shaders.
struct DrawRoot {
    float color[4];
    float depth;
};
// --8<-- [end:root-struct]

} // namespace

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});

    // --8<-- [start:format-supported]
    // D24S8 is missing on some GPUs (AMD on Vulkan): ask, and fall back.
    const RI::TextureUsage depthUsage = RI::TextureUsage::eDepthStencil | RI::TextureUsage::eSampled;
    const RI::Format depthFormat = RI::formatSupported(device, RI::Format::eD24UnormS8Uint, depthUsage)
                                       ? RI::Format::eD24UnormS8Uint
                                       : RI::Format::eD32FloatS8Uint;
    // MSAA: the sample count is part of the question.
    const bool msaa4 = RI::formatSupported(device, RI::Format::eRGBA16Float, RI::TextureUsage::eRenderTarget, 4);
    // --8<-- [end:format-supported]
    std::printf("depth format %u, 4x MSAA: %s\n", static_cast<unsigned>(depthFormat), msaa4 ? "yes" : "no");

    constexpr uint32 kWidth = 1280;
    constexpr uint32 kHeight = 720;

    // --8<-- [start:texture-desc]
    // A color target that shaders also sample, with an sRGB view format (P74).
    const RI::TextureDesc colorDesc{
        .width = kWidth,
        .height = kHeight,
        .format = RI::Format::eRGBA8Unorm,
        .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled,
        .viewFormats = {RI::Format::eRGBA8Srgb},
    };
    // A depth-stencil target that shaders also read (soft particles, SSAO, ...).
    const RI::TextureDesc depthDesc{.width = kWidth, .height = kHeight, .format = depthFormat, .usage = depthUsage};
    // A texture compute shaders write, with a full mip chain.
    const RI::TextureDesc storageDesc{
        .width = 512,
        .height = 512,
        .mips = 10,
        .format = RI::Format::eRGBA16Float,
        .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eSampled,
    };
    // --8<-- [end:texture-desc]

    // --8<-- [start:requirements]
    // Size, alignment and the memory kinds that can hold each texture.
    const RI::MemoryRequirements colorReq = RI::textureRequirements(device, colorDesc);
    const RI::MemoryRequirements depthReq = RI::textureRequirements(device, depthDesc);
    const RI::MemoryRequirements storageReq = RI::textureRequirements(device, storageDesc);
    if (!RI::fits(colorReq, RI::MemoryKind::eGPU)) {
        return 1; // textures live in eGPU memory on every GPU RI supports
    }
    const uint64 depthOffset = alignUp(colorReq.size, depthReq.alignment);
    const uint64 storageOffset = alignUp(depthOffset + depthReq.size, storageReq.alignment);
    const uint64 storageEnd = storageOffset + storageReq.size;
    // --8<-- [end:requirements]

    // Two scratch textures that share memory (aliasing), after the others.
    const RI::TextureDesc scratchDesc{.width = kWidth,
                                      .height = kHeight,
                                      .format = RI::Format::eRGBA16Float,
                                      .usage = RI::TextureUsage::eRenderTarget | RI::TextureUsage::eSampled};
    const RI::MemoryRequirements scratchReq = RI::textureRequirements(device, scratchDesc);
    const uint64 scratchOffset = alignUp(storageEnd, scratchReq.alignment);

    // --8<-- [start:create-textures]
    RI::Memory memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, scratchOffset + scratchReq.size);
    RI::Texture color = RI::createTexture(device, memory, 0, colorDesc);
    RI::Texture depth = RI::createTexture(device, memory, depthOffset, depthDesc);
    RI::Texture storage = RI::createTexture(device, memory, storageOffset, storageDesc);
    // Each texture comes with a view of the whole texture in its own format: color.view.
    // --8<-- [end:create-textures]

    // --8<-- [start:create-aliased]
    // Two textures over the same bytes: use one, then the other, never both at once.
    RI::Texture blurH = RI::createTexture(device, memory, scratchOffset, scratchDesc);
    RI::Texture blurV = RI::createTexture(device, memory, scratchOffset, scratchDesc);
    // --8<-- [end:create-aliased]

    // --8<-- [start:create-views]
    // The same texels read as sRGB (listed in viewFormats).
    RI::TextureView colorSrgb = RI::createView(device, color, {.format = RI::Format::eRGBA8Srgb});
    // Shaders read depth or stencil, never both: a depth-only view.
    RI::TextureView depthOnly = RI::createView(device, depth, {.aspect = RI::Aspect::eDepth});
    // One mip, e.g. to write mip 1 while sampling mip 0.
    RI::TextureView storageMip1 = RI::createView(device, storage, {.baseMip = 1, .mipCount = 1});
    // storageMip1.width == 256: a view reports the size of its first mip.
    // --8<-- [end:create-views]

    // --8<-- [start:write-descriptors]
    // Texture views go in heap slots like buffer views, through `.texture`.
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 16});
    RI::writeDescriptors(device, heap,
                         {{.slot = 0, .descriptor = RI::ReadView{.texture = colorSrgb}},      // Texture2D<float4>
                          {.slot = 1, .descriptor = RI::ReadView{.texture = depthOnly}},      // Texture2D<float>
                          {.slot = 2, .descriptor = RI::ReadWriteView{.texture = storageMip1}}}); // RWTexture2D<float4>
    // --8<-- [end:write-descriptors]

    // --8<-- [start:create-pipeline]
    const std::vector<std::byte> vertexShader = loadShader("doc_triangle_vs");
    const std::vector<std::byte> pixelShader = loadShader("doc_triangle_ps");
    // Complete state up front (P38): shaders, attachment formats, depth state.
    RI::Pipeline pipeline = RI::createGraphicsPipeline(device, {
        .vertex = vertexShader,
        .pixel = pixelShader,
        .colorFormats = {RI::Format::eRGBA8Unorm},
        .depthFormat = depthFormat,
        .cull = RI::CullMode::eBack,
        .depth = {.test = true, .write = true, .compare = RI::CompareOp::eLess},
    });
    // --8<-- [end:create-pipeline]

    // --8<-- [start:create-pipeline-depth-only]
    // Depth only: no pixel shader, no color format.
    RI::Pipeline depthPrepass = RI::createGraphicsPipeline(device, {
        .vertex = vertexShader,
        .depthFormat = depthFormat,
        .depth = {.test = true, .write = true, .compare = RI::CompareOp::eLess},
    });
    // --8<-- [end:create-pipeline-depth-only]

    // --8<-- [start:create-query-pool]
    // A pool of timestamp queries; it starts reset.
    RI::QueryPool timestamps = RI::createQueryPool(device, {.type = RI::QueryType::eTimestamp, .count = 4});
    // Results go to a buffer: here readback memory, read by the CPU.
    RI::Memory readback = RI::allocateMemory(device, RI::MemoryKind::eReadback, 4096);
    RI::Buffer ticks = RI::createBuffer(device, readback, 0, {.size = 4 * sizeof(uint64), .usage = RI::BufferUsage::eCopyDst});
    // --8<-- [end:create-query-pool]

    // A GPU buffer that cmdClearBuffer resets (eCopyDst; eGPU memory, see the rules).
    RI::Memory bufferMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 65536);
    RI::Buffer counters = RI::createBuffer(
        device, bufferMemory, 0, {.size = 4096, .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopyDst});

    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);

    // --8<-- [start:write-timestamp-begin]
    RI::cmdWriteTimestamp(cmd, timestamps, 0); // when all earlier work is done
    // --8<-- [end:write-timestamp-begin]

    // --8<-- [start:rendering]
    // --8<-- [start:begin-rendering]
    // Clear both attachments at the start of the section and keep the results.
    RI::cmdBeginRendering(cmd, {
        .colors = {{.view = color.view, .load = RI::LoadOp::eClear, .clear = {.color = {0.1f, 0.1f, 0.1f, 1.0f}}}},
        .depth = RI::DepthAttachment{.view = depth.view,
                                     .load = RI::LoadOp::eClear,
                                     .stencilLoad = RI::LoadOp::eClear,
                                     .clear = {.depth = 1.0f, .stencil = 0}},
    });
    // --8<-- [end:begin-rendering]
    // --8<-- [start:draw]
    RI::cmdSetPipeline(cmd, pipeline);
    RI::cmdSetRootData(cmd, DrawRoot{.color = {1.0f, 0.5f, 0.0f, 1.0f}, .depth = 0.5f});
    RI::cmdDraw(cmd, 3); // three vertices, one instance
    // --8<-- [end:draw]
    // --8<-- [start:end-rendering]
    RI::cmdEndRendering(cmd);
    // --8<-- [end:end-rendering]
    // --8<-- [end:rendering]

    // --8<-- [start:barrier-render-sample]
    // The section's writes, then shaders that sample color and depth.
    RI::cmdBarrier(cmd, RI::Stage::eRenderTarget | RI::Stage::eDepthStencil, RI::Stage::eCompute | RI::Stage::ePixel);
    // --8<-- [end:barrier-render-sample]

    // --8<-- [start:read-only-depth]
    // Depth stays bound for testing while shaders sample it (P75). Draws in
    // this section must not write depth; loads keep the contents.
    RI::cmdBeginRendering(cmd, {
        .colors = {{.view = color.view}}, // eLoad / eStore: keep what is there
        .depth = RI::DepthAttachment{.view = depth.view, .depthReadOnly = true, .stencilReadOnly = true},
    });
    // ... draws whose pipeline has depth.write == false, sampling depthOnly ...
    RI::cmdEndRendering(cmd);
    // --8<-- [end:read-only-depth]

    // --8<-- [start:clears]
    // Clears outside rendering sections (P65). They count as copies for barriers.
    RI::cmdClearTexture(cmd, storage.view, {.color = {0.0f, 0.0f, 0.0f, 0.0f}});
    RI::cmdClearBuffer(cmd, counters, 0); // every 32-bit word set to 0
    RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCompute);
    // --8<-- [end:clears]

    // --8<-- [start:discard]
    // blurH and blurV share memory. Before each one's first use in a pass,
    // discard it: its contents (and hidden compression data) are reset (P69).
    RI::cmdDiscard(cmd, blurH.view);
    RI::cmdBeginRendering(cmd, {.colors = {{.view = blurH.view, .load = RI::LoadOp::eDontCare}}});
    // ... draws ...
    RI::cmdEndRendering(cmd);
    // blurH's contents are no longer needed: blurV takes over the memory.
    RI::cmdDiscard(cmd, blurV.view);
    RI::cmdBeginRendering(cmd, {.colors = {{.view = blurV.view, .load = RI::LoadOp::eDontCare}}});
    // ... draws ...
    RI::cmdEndRendering(cmd);
    // --8<-- [end:discard]

    // --8<-- [start:write-timestamp-end]
    RI::cmdWriteTimestamp(cmd, timestamps, 1);
    // Copy both results (64-bit each) to the buffer.
    RI::cmdResolveQueries(cmd, timestamps, 0, 2, ticks, 0);
    // --8<-- [end:write-timestamp-end]
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));

    // --8<-- [start:read-timestamps]
    // After the timeline point: ticks to milliseconds with the device's frequency.
    const auto *values = static_cast<const uint64 *>(ticks.mapped);
    const double ms = static_cast<double>(values[1] - values[0]) * 1000.0 /
                      static_cast<double>(device.limits.timestampFrequency);
    std::printf("GPU time: %.3f ms\n", ms);
    // Reset the queries before writing them again (a CPU call, no GPU cost).
    RI::resetQueries(device, timestamps, 0, 2);
    // --8<-- [end:read-timestamps]

    // --8<-- [start:reset-in-list]
    // Or reset them in a command list, outside rendering sections.
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdResetQueries(cmd, timestamps, 2, 2);
    RI::cmdWriteTimestamp(cmd, timestamps, 2);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    // --8<-- [end:reset-in-list]

    // --8<-- [start:destroy]
    // The GPU is done (the point was reached). Views you created first, then textures
    // (which destroy their whole-texture view), then the memory.
    RI::destroyCommandList(device, cmd);
    RI::destroyQueryPool(device, timestamps);
    RI::destroyBuffer(device, ticks);
    RI::freeMemory(device, readback);
    RI::destroyBuffer(device, counters);
    RI::freeMemory(device, bufferMemory);
    RI::destroyPipeline(device, depthPrepass);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyView(device, colorSrgb);
    RI::destroyView(device, depthOnly);
    RI::destroyView(device, storageMip1);
    for (RI::Texture *texture : {&color, &depth, &storage, &blurH, &blurV}) {
        RI::destroyTexture(device, *texture);
    }
    RI::freeMemory(device, memory);
    RI::destroyDevice(device);
    // --8<-- [end:destroy]
    return 0;
}
