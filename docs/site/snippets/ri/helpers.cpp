// RI helpers (P41): memory and slot allocators, transfers, a frame ring with
// a profiler and the shader compiler. Compiled once
// per enabled backend.

#include <RI/Helpers/FrameRing.hpp>
#include <RI/Helpers/MemoryAllocator.hpp>
#include <RI/Helpers/Profiler.hpp>
#include <RI/Helpers/ShaderCompiler.hpp>
#include <RI/Helpers/SlotAllocator.hpp>
#include <RI/Helpers/Transfers.hpp>
#include <RI/RI.hpp>

#include <cstdio>
#include <vector>

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty()) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});

    // --8<-- [start:allocator]
    RI::Helpers::MemoryAllocator allocator = RI::Helpers::createMemoryAllocator(device);
    RI::Helpers::Allocation bufferAllocation;
    RI::Buffer buffer = RI::Helpers::createBuffer(
        allocator, RI::MemoryKind::eGPU,
        {.size = 1 << 20, .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopyDst}, bufferAllocation);
    RI::Helpers::Allocation textureAllocation;
    RI::Texture texture = RI::Helpers::createTexture(allocator,
                                                     {.width = 256,
                                                      .height = 256,
                                                      .format = RI::Format::eRGBA8Unorm,
                                                      .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                                               RI::TextureUsage::eCopyDst},
                                                     textureAllocation);
    // When textureAllocation.needsDiscard is set, the range held another
    // resource before: cmdDiscard the texture before its first use (P69).
    // --8<-- [end:allocator]

    // --8<-- [start:transfers]
    const std::vector<uint32> image(256 * 256, 0xff00ff00u); // rows tightly packed
    RI::Helpers::uploadTexture(device, texture.view, image.data());
    const std::vector<std::byte> texels = RI::Helpers::readbackTexture(device, texture.view);
    // --8<-- [end:transfers]
    (void)texels;

    // --8<-- [start:slots]
    // Slots 16 .. 1023 of the resource heap; 0 .. 15 stay yours.
    RI::Helpers::SlotAllocator slots = RI::Helpers::createSlotAllocator(device.graphicsQueue, 16, 1008);
    const uint32 slot = RI::Helpers::allocateSlot(slots);
    // ... write a descriptor to `slot`, record and submit work that reads it ...
    // --8<-- [end:slots]

    // --8<-- [start:frame-ring]
    RI::Helpers::FrameRing ring = RI::Helpers::createFrameRing(device, 2);
    RI::Helpers::Profiler profiler = RI::Helpers::createProfiler(device);
    RI::TimelinePoint lastFrame;
    for (int frame = 0; frame < 3; ++frame) {
        RI::CommandList &cmd = RI::Helpers::beginFrame(ring); // waits only if this list is in flight
        RI::Helpers::beginScope(profiler, cmd, "frame");
        RI::Helpers::beginScope(profiler, cmd, "clear");
        RI::cmdClearTexture(cmd, texture.view, {.color = {0.0f, 0.0f, 0.0f, 1.0f}});
        RI::Helpers::endScope(profiler, cmd);
        RI::Helpers::endScope(profiler, cmd);
        RI::Helpers::resolveProfiler(profiler, cmd);
        lastFrame = RI::Helpers::endFrame(ring);
        // The times are readable once the frame's point is reached.
        RI::wait(device.graphicsQueue, lastFrame);
        for (const RI::Helpers::ScopeTime &scope : RI::Helpers::profilerResults(profiler)) {
            std::printf("%*s%s: %.3f ms\n", int(scope.depth * 2), "", scope.name.c_str(), scope.milliseconds);
        }
    }
    // --8<-- [end:frame-ring]

    // --8<-- [start:release]
    // Free the slot once the last frame that may read it is done.
    RI::Helpers::releaseSlot(slots, slot, lastFrame);
    // --8<-- [end:release]

    // --8<-- [start:compiler]
    RI::Helpers::ShaderCompiler compiler = RI::Helpers::createShaderCompiler(); // the build's pinned DXC
    const char *code = R"(#include <equine/root.hlsli>
struct Root { uint slot; float value; };
EQ_ROOT_DATA(Root, root);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[root.slot];
    output[id.x] = root.value * SCALE;
})";
    const RI::Helpers::CompiledShader shader =
        RI::Helpers::compileShader(compiler, {.code = code, .profile = "cs_6_6", .defines = {"SCALE=2.0"}});
    if (shader.bytecode.empty()) {
        std::printf("%s", shader.messages.c_str());
        return 1;
    }
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader.bytecode});
    // --8<-- [end:compiler]

    RI::destroyPipeline(device, pipeline);
    RI::Helpers::destroyShaderCompiler(compiler);
    RI::Helpers::destroyProfiler(profiler);
    RI::Helpers::destroyFrameRing(ring);
    RI::destroyTexture(device, texture);
    RI::Helpers::release(allocator, textureAllocation);
    RI::destroyBuffer(device, buffer);
    RI::Helpers::release(allocator, bufferAllocation);
    RI::Helpers::destroyMemoryAllocator(allocator);
    RI::destroyDevice(device);
    return 0;
}
