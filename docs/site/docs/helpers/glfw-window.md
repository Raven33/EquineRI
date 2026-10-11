---
symbols: [RI::Helpers::windowOf]
---

# GLFW window helper

## What it does

RI creates no windows ([P43](../design-notes/decisions.md#p43)). This helper turns a window
made with [GLFW](https://www.glfw.org) into the [`RI::Window`](../reference/Window.md) a
[swapchain](../reference/swapchain.md) needs: `RI::Helpers::windowOf(glfwWindow)` returns the
OS handles of the platform GLFW runs on (Win32, X11 or Wayland).

Create the window with `GLFW_CLIENT_API` set to `GLFW_NO_API`: RI, not GLFW, talks to the GPU.

The helper is the library `EquineGlfwWindow` (header `<RI/Helpers/GlfwWindow.hpp>`); GLFW 3.4
is a pinned submodule (`third_party/glfw`).

## Example

```cpp
--8<-- "snippets/ri/swapchain.cpp:create"
```

## How to roll your own

The helper is a few lines over `glfw3native.h`; with another window library, fill
`RI::Window` the same way:

| Platform | `system` | `display` | `handle` / `id` |
|---|---|---|---|
| Windows | `eWin32` | `GetModuleHandleW(nullptr)` | `handle`: the `HWND` |
| X11 | `eXlib` | the `Display*` | `id`: the X11 `Window` |
| Wayland | `eWayland` | the `wl_display*` | `handle`: the `wl_surface*` |
