// Swapchains (P43, P63, P64): a real window from the GLFW helper, a few frames
// cleared and presented, a copy into the screen texture, then a resize. Skips
// when no window can be opened (no display).

#include "GpuTestCommon.hpp"

#include <TestHarness.hpp>

#include <RI/Helpers/GlfwWindow.hpp>
#include <RI/RI.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

using gputest::Arena;
using gputest::createTestDevice;
using gputest::placeBuffer;

// Records one frame into the screen texture: wait for the list's last
// submission (P56), clear, optionally copy `upload` in, submit, present.
void drawFrame(RI::Device &device, RI::Swapchain &swapchain, RI::CommandList &cmd, RI::TimelinePoint &last,
               float value, const RI::Buffer *upload) {
    glfwPollEvents();
    RI::Texture screen = RI::acquire(device, swapchain);
    EQ_CHECK(screen.width == swapchain.width && screen.height == swapchain.height);
    EQ_CHECK((screen.usage & RI::TextureUsage::eRenderTarget) != RI::TextureUsage::eNone);
    RI::wait(device.graphicsQueue, last);
    RI::resetCommandList(cmd);
    RI::cmdBegin(cmd);
    RI::cmdBeginRendering(cmd, {.colors = {{.view = screen.view,
                                             .load = RI::LoadOp::eClear,
                                             .clear = {.color = {value, 0.2f, 1.0f - value, 1.0f}}}}});
    RI::cmdEndRendering(cmd);
    if (upload != nullptr) {
        // A copy into the screen texture (eCopyDst), e.g. an emulated screen (P64).
        RI::cmdBarrier(cmd, RI::Stage::eRenderTarget, RI::Stage::eCopy);
        RI::cmdCopyBufferToTexture(cmd, *upload, 0, screen.view);
    }
    RI::cmdEnd(cmd);
    last = RI::submit(device.graphicsQueue, cmd);
    RI::present(device.graphicsQueue, swapchain);
}

void swapchainOn(int platform) {
    if (platform != GLFW_ANY_PLATFORM && glfwPlatformSupported(platform) != GLFW_TRUE) {
        std::printf("platform not supported by GLFW here: skipped\n");
        return;
    }
    glfwInitHint(GLFW_PLATFORM, platform);
    // No window decorations: libdecor's GTK plugin is not needed for the test.
    glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
    if (glfwInit() != GLFW_TRUE) {
        std::printf("no display: skipped\n");
        return;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(256, 192, "EquineRI swapchain test", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        std::printf("no window: skipped\n");
        return;
    }
    RI::Device device = createTestDevice();
    const RI::Window osWindow = RI::Helpers::windowOf(window);
    const std::vector<RI::SurfaceFormat> formats = RI::surfaceFormats(device, osWindow);
    EQ_CHECK(!formats.empty());
    const RI::SurfaceFormat wanted{RI::Format::eBGRA8Srgb, RI::ColorSpace::eSrgb};
    const RI::SurfaceFormat format = std::ranges::find(formats, wanted) != formats.end() ? wanted : formats.front();
    EQ_CHECK(RI::presentModeSupported(device, osWindow, RI::PresentMode::eFifo));
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    RI::Swapchain swapchain = RI::createSwapchain(device, {.window = osWindow,
                                                           .width = static_cast<uint32>(width),
                                                           .height = static_cast<uint32>(height),
                                                           .format = format});
    EQ_CHECK(swapchain.imageCount >= 2 && swapchain.width > 0 && swapchain.height > 0);

    Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 16 << 20)};
    RI::CommandList cmd = RI::createCommandList(device);
    RI::TimelinePoint last;
    RI::Buffer pixels;
    for (uint32 frame = 0; frame < 3; ++frame) {
        if (frame == 1) {
            // The copy needs the screen texture's layout: ask with any screen texture.
            RI::Texture screen = RI::acquire(device, swapchain);
            const RI::CopyLayout layout = RI::copyLayout(device, screen.view);
            pixels = placeBuffer(device, upload, {.size = layout.size, .usage = RI::BufferUsage::eCopySrc});
            RI::wait(device.graphicsQueue, last);
            RI::resetCommandList(cmd);
            RI::cmdBegin(cmd);
            RI::cmdEnd(cmd);
            last = RI::submit(device.graphicsQueue, cmd);
            RI::present(device.graphicsQueue, swapchain);
        }
        drawFrame(device, swapchain, cmd, last, 0.3f * static_cast<float>(frame), frame == 1 ? &pixels : nullptr);
    }
    RI::resizeSwapchain(device, swapchain, static_cast<uint32>(width) / 2, static_cast<uint32>(height) / 2);
    for (uint32 frame = 0; frame < 2; ++frame) {
        drawFrame(device, swapchain, cmd, last, 0.5f, nullptr);
    }
    RI::wait(device.graphicsQueue, last);

    RI::destroyBuffer(device, pixels);
    RI::destroyCommandList(device, cmd);
    RI::freeMemory(device, upload.memory);
    RI::destroySwapchain(device, swapchain);
    RI::destroyDevice(device);
    glfwDestroyWindow(window);
    glfwTerminate();
}

} // namespace

#if defined(_WIN32)
EQ_TEST(window, swapchain) { swapchainOn(GLFW_PLATFORM_WIN32); }
#else
EQ_TEST(window, swapchain_x11) { swapchainOn(GLFW_PLATFORM_X11); }
EQ_TEST(window, swapchain_wayland) { swapchainOn(GLFW_PLATFORM_WAYLAND); }
#endif
