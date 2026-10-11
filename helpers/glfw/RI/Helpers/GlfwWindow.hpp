#pragma once

// GLFW window helper (P43, P41): the RI::Window of a GLFW window, for
// createSwapchain. Built only on RI's portable surface; replace it with your
// own window code at will (see the helper's docs page).

#include <RI/Window.hpp>

struct GLFWwindow;

namespace RI::Helpers {

// The OS handles of `window`, for the platform GLFW runs on (Win32, X11 or
// Wayland). Create the window with GLFW_CLIENT_API set to GLFW_NO_API.
Window windowOf(GLFWwindow *window);

} // namespace RI::Helpers
