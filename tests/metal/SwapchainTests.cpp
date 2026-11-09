// Metal-only swapchain tests (P43, P63, P64): what the screen texture holds,
// read back through Metal itself, since RI copies nothing out of a screen
// texture. Over SSH, with no GUI session, GLFW still opens a window on no
// display and the layer still gives drawables: the tests run, nothing shows.

#include <TestHarness.hpp>

#include "../gpu/GlfwWindow.hpp"
#include "../gpu/GpuTestCommon.hpp"

#include <RI/Helpers/TypedCommands.hpp>
#include <RI/Metal/Native.hpp>
#include <RI/RI.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using gputest::createTestDevice;

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

// The texture's bytes (4 per texel), copied by a blit on a queue of its own.
std::vector<uint8> readNative(MTL::Device *device, MTL::Texture *texture) {
    NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
    const NS::UInteger width = texture->width(), height = texture->height();
    MTL::CommandQueue *queue = device->newCommandQueue();
    MTL::Buffer *buffer = device->newBuffer(width * height * 4, MTL::ResourceStorageModeShared);
    MTL::CommandBuffer *commands = queue->commandBuffer();
    MTL::BlitCommandEncoder *blit = commands->blitCommandEncoder();
    blit->copyFromTexture(texture, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), buffer, 0, width * 4,
                          width * height * 4);
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    std::vector<uint8> bytes(width * height * 4);
    std::memcpy(bytes.data(), buffer->contents(), bytes.size());
    buffer->release();
    queue->release();
    pool->release();
    return bytes;
}

// BGRA bytes of texel (x, y) as 0xBBGGRRAA.
uint32 texel(const std::vector<uint8> &bytes, uint32 width, uint32 x, uint32 y) {
    const uint8 *p = &bytes[(y * width + x) * 4];
    return (uint32{p[0]} << 24) | (uint32{p[1]} << 16) | (uint32{p[2]} << 8) | p[3];
}

} // namespace

// A clear, then a compute write (storage, P64 (5)), each read back before
// present; and the capabilities the layer reports.
EQ_TEST(metal, screen_contents) {
    if (glfwInit() != GLFW_TRUE) {
        std::printf("no window system: skipped\n");
        return;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(64, 48, "EquineRI screen contents", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        std::printf("no window: skipped\n");
        return;
    }
    RI::Device device = createTestDevice();
    const RI::Window osWindow = gputest::windowOf(window);
    EQ_CHECK(!RI::presentModeSupported(device, osWindow, RI::PresentMode::eMailbox));
    EQ_CHECK(RI::presentModeSupported(device, osWindow, RI::PresentMode::eImmediate));
    EQ_CHECK(!RI::screenStorageSupported(device, osWindow, RI::Format::eBGRA8Srgb));
    EQ_CHECK(RI::screenStorageSupported(device, osWindow, RI::Format::eBGRA8Unorm));

    constexpr uint32 kWidth = 40, kHeight = 24; // the drawable's size, not the window's
    RI::Swapchain swapchain = RI::createSwapchain(device, {.window = osWindow,
                                                           .width = kWidth,
                                                           .height = kHeight,
                                                           .format = {RI::Format::eBGRA8Unorm, RI::ColorSpace::eSrgb},
                                                           .imageCount = 2,
                                                           .storage = true,
                                                           .name = "screen"});
    EQ_CHECK(swapchain.imageCount == 2);
    MTL::Device *native = RI::Metal::native(device).device;
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4, .samplerCount = 0});
    const std::vector<std::byte> shader = gputest::loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(device, {.shader = shader});
    RI::CommandList cmd = RI::createCommandList(device);

    // A clear inside a rendering section.
    RI::Texture screen = RI::acquire(device, swapchain);
    EQ_CHECK(screen.width == kWidth && screen.height == kHeight);
    EQ_CHECK((screen.usage & RI::TextureUsage::eStorage) != RI::TextureUsage::eNone);
    RI::cmdBegin(cmd);
    RI::cmdBeginRendering(cmd, {.colors = {{.view = screen.view,
                                             .load = RI::LoadOp::eClear,
                                             .clear = {.color = {0.25f, 0.5f, 0.75f, 1.0f}}}}});
    RI::cmdEndRendering(cmd);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    std::vector<uint8> bytes = readNative(native, RI::Metal::native(screen));
    EQ_CHECK(texel(bytes, kWidth, 0, 0) == 0xbf8040ff && texel(bytes, kWidth, kWidth - 1, kHeight - 1) == 0xbf8040ff);
    RI::present(device.graphicsQueue, swapchain);

    // A compute write: (x, y, 0.5, 1), clamped by the UNORM format.
    screen = RI::acquire(device, swapchain);
    RI::writeDescriptors(device, heap, {{.slot = 1, .descriptor = RI::ReadWriteView{.texture = screen.view}}});
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, write);
    RI::Helpers::cmdSetRootData(cmd, WriteRoot{.textureSlot = 1, .width = kWidth, .height = kHeight, .value = 0.5f});
    RI::cmdDispatch(cmd, kWidth / 8, kHeight / 8, 1);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    bytes = readNative(native, RI::Metal::native(screen));
    EQ_CHECK(texel(bytes, kWidth, 0, 0) == 0x800000ff);
    EQ_CHECK(texel(bytes, kWidth, 3, 5) == 0x80ffffff);
    EQ_CHECK(texel(bytes, kWidth, 1, 0) == 0x8000ffff);
    RI::present(device.graphicsQueue, swapchain);

    // The next frame's texture after a resize has the new size.
    RI::resizeSwapchain(device, swapchain, kWidth / 2, kHeight / 2);
    screen = RI::acquire(device, swapchain);
    EQ_CHECK(screen.width == kWidth / 2 && screen.height == kHeight / 2);
    EQ_CHECK(RI::Metal::native(screen)->width() == kWidth / 2);
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    RI::present(device.graphicsQueue, swapchain);

    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, write);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroySwapchain(device, swapchain);
    RI::destroyDevice(device);
    glfwDestroyWindow(window);
    glfwTerminate();
}
