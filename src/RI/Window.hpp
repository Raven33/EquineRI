#pragma once

// A window as the operating system knows it (CONTEXT.md P43): plain OS handles,
// never native graphics objects. RI creates no window; the user (or the GLFW
// window helper) creates it and passes its handles when creating a swapchain.
// Also the portable swapchain descriptions (P43, P63, P64).

#include <RI/Textures.hpp>
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

// How the screen interprets the swapchain's values (P64).
enum class ColorSpace : uint8 {
    eSrgb,  // SDR, sRGB primaries and transfer
    eHdr10, // HDR10: BT.2020 primaries, PQ (ST 2084) transfer, typically eRGB10A2Unorm
    eScRgb, // extended linear sRGB, typically eRGBA16Float
};

struct SurfaceFormat {
    Format format = Format::eBGRA8Srgb;
    ColorSpace colorSpace = ColorSpace::eSrgb;
    bool operator==(const SurfaceFormat &) const = default;
};

// eFifo: waits for vertical blank (always available). eMailbox: no tearing,
// the newest frame replaces a waiting one. eImmediate: no waiting, may tear.
enum class PresentMode : uint8 { eFifo, eMailbox, eImmediate };

struct SwapchainDesc {
    Window window;
    uint32 width = 0; // the window's client size in pixels
    uint32 height = 0;
    SurfaceFormat format; // one of surfaceFormats(device, window)
    PresentMode presentMode = PresentMode::eFifo;
    uint32 imageCount = 0; // 0: RI's default for the present mode (P43)
    // Compute writes to the screen texture (eStorage): only where
    // screenStorageSupported says so; never on D3D12 (P64 (5)).
    bool storage = false;
};

} // namespace RI
