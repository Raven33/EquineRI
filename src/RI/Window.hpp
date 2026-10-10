#pragma once

// A window as the operating system knows it (CONTEXT.md P43): plain OS handles,
// never native graphics objects. RI creates no window; the user (or the GLFW
// window helper) creates it and passes its handles when creating a swapchain.

#include <types.hpp>

namespace RI {

enum class WindowSystem : uint8 {
    eWin32,   // display: HINSTANCE, window: HWND
    eXlib,    // display: Display*, window: the X11 Window id
    eXcb,     // display: xcb_connection_t*, window: the xcb_window_t id
    eWayland, // display: wl_display*, window: wl_surface*
};

struct Window {
    WindowSystem system = WindowSystem::eWin32;
    void *display = nullptr;
    void *handle = nullptr; // pointer-sized handles (HWND, wl_surface*)
    uint64 id = 0;          // integer ids (X11 Window, xcb_window_t)
};

} // namespace RI
