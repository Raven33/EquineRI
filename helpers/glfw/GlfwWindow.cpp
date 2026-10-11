#include <RI/Helpers/GlfwWindow.hpp>

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

namespace RI::Helpers {

Window windowOf(GLFWwindow *window) {
#if defined(_WIN32)
    return {.system = WindowSystem::eWin32, .display = GetModuleHandleW(nullptr), .handle = glfwGetWin32Window(window)};
#else
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return {.system = WindowSystem::eWayland,
                .display = glfwGetWaylandDisplay(),
                .handle = glfwGetWaylandWindow(window)};
    }
    return {.system = WindowSystem::eXlib, .display = glfwGetX11Display(), .id = glfwGetX11Window(window)};
#endif
}

} // namespace RI::Helpers
