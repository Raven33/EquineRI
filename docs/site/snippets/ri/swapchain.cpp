// RI: a window, a swapchain and a frame loop (P43, P63, P64), the window
// opened with GLFW. Compiled once per enabled backend.

#include <RI/RI.hpp>

// --8<-- [start:glfw]
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

// The OS handles of a GLFW window, on the platform GLFW runs on.
RI::Window windowOf(GLFWwindow *window) {
#if defined(_WIN32)
    return {.system = RI::WindowSystem::eWin32, .display = GetModuleHandleW(nullptr), .handle = glfwGetWin32Window(window)};
#elif defined(__APPLE__)
    return {.system = RI::WindowSystem::eCocoa, .handle = glfwGetCocoaView(window)};
#else
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return {.system = RI::WindowSystem::eWayland,
                .display = glfwGetWaylandDisplay(),
                .handle = glfwGetWaylandWindow(window)};
    }
    return {.system = RI::WindowSystem::eXlib, .display = glfwGetX11Display(), .id = glfwGetX11Window(window)};
#endif
}
// --8<-- [end:glfw]

#include <algorithm>
#include <vector>

int main() {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    if (gpus.empty() || glfwInit() != GLFW_TRUE) {
        return 1;
    }
    RI::Device device = RI::createDevice({.gpu = gpus[0]});

    // --8<-- [start:create]
    // RI creates no window: GLFW does, and windowOf gives RI its OS handles.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "EquineRI", nullptr, nullptr);
    const RI::Window osWindow = windowOf(window);

    // Pick a format the window shows: sRGB 8-bit if possible (P64).
    const std::vector<RI::SurfaceFormat> formats = RI::surfaceFormats(device, osWindow);
    const RI::SurfaceFormat srgb{RI::Format::eBGRA8Srgb, RI::ColorSpace::eSrgb};
    const RI::SurfaceFormat format = std::ranges::find(formats, srgb) != formats.end() ? srgb : formats.front();
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    RI::Swapchain swapchain = RI::createSwapchain(device, {.window = osWindow,
                                                           .width = static_cast<uint32>(width),
                                                           .height = static_cast<uint32>(height),
                                                           .format = format,
                                                           .presentMode = RI::PresentMode::eFifo});
    // --8<-- [end:create]

    RI::CommandList cmd = RI::createCommandList(device);
    RI::TimelinePoint last;
    // --8<-- [start:frame]
    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        glfwPollEvents();
        int newWidth = 0, newHeight = 0;
        glfwGetFramebufferSize(window, &newWidth, &newHeight);
        if (newWidth == 0 || newHeight == 0) {
            continue; // minimized
        }
        if (static_cast<uint32>(newWidth) != swapchain.width || static_cast<uint32>(newHeight) != swapchain.height) {
            RI::resizeSwapchain(device, swapchain, static_cast<uint32>(newWidth), static_cast<uint32>(newHeight));
        }
        // The screen texture of this frame: draw into it like any texture.
        RI::Texture screen = RI::acquire(device, swapchain);
        RI::wait(device.graphicsQueue, last); // one command list: wait before reusing it
        RI::resetCommandList(cmd);
        RI::cmdBegin(cmd);
        RI::cmdBeginRendering(cmd, {.colors = {{.view = screen.view,
                                                 .load = RI::LoadOp::eClear,
                                                 .clear = {.color = {0.1f, 0.2f, 0.4f, 1.0f}}}}});
        // ... draws ...
        RI::cmdEndRendering(cmd);
        RI::cmdEnd(cmd);
        last = RI::submit(device.graphicsQueue, cmd);
        RI::present(device.graphicsQueue, swapchain); // after the work submitted before it
    }
    // Before destroying anything the frames used, wait for the last one.
    RI::wait(device.graphicsQueue, last);
    // --8<-- [end:frame]

    RI::destroyCommandList(device, cmd);
    RI::destroySwapchain(device, swapchain);
    RI::destroyDevice(device);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
