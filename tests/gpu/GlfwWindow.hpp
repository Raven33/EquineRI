#pragma once

// The RI::Window of a GLFW window, for the swapchain tests (P43, P109): RI
// ships no window helper; this is the code a user writes (see the RI::Window
// page). Create the window with GLFW_CLIENT_API set to GLFW_NO_API.

#include <RI/Window.hpp>

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

namespace gputest {

inline RI::Window windowOf(GLFWwindow *window) {
#if defined(_WIN32)
    return {.system = RI::WindowSystem::eWin32, .display = GetModuleHandleW(nullptr), .handle = glfwGetWin32Window(window)};
#else
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return {.system = RI::WindowSystem::eWayland,
                .display = glfwGetWaylandDisplay(),
                .handle = glfwGetWaylandWindow(window)};
    }
    return {.system = RI::WindowSystem::eXlib, .display = glfwGetX11Display(), .id = glfwGetX11Window(window)};
#endif
}

} // namespace gputest
