---
symbols: [RI::Window, RI::WindowSystem]
---

# RI::Window

## What it is

`RI::Window` describes a window as the operating system knows it: plain OS handles, never a
native graphics object. RI creates no windows and ships no window library
([P43](../design-notes/decisions.md#p43), [P109](../design-notes/decisions.md#p109)). You
create the window with the library of your choice and fill in this struct; [`createSwapchain`](swapchain.md) takes it to present to the window.

| `system` | `display` | `handle` | `id` |
|---|---|---|---|
| `WindowSystem::eWin32` | `HINSTANCE` | `HWND` | unused |
| `WindowSystem::eXlib` | `Display*` | unused | the X11 `Window` |
| `WindowSystem::eXcb` | `xcb_connection_t*` | unused | the `xcb_window_t` |
| `WindowSystem::eWayland` | `wl_display*` | `wl_surface*` | unused |

Pointer-sized handles go in `handle`; integer ids (X11) go in `id`, so no cast is needed.

## Signature

--8<-- "generated/signatures/RI.Window.md"

--8<-- "generated/signatures/RI.WindowSystem.md"

## Example

```cpp
// Win32: the window you created with CreateWindowExW.
const RI::Window window{
    .system = RI::WindowSystem::eWin32,
    .display = GetModuleHandleW(nullptr),
    .handle = hwnd,
};
```

### From a GLFW window

A few lines over `glfw3native.h`; create the window with `GLFW_CLIENT_API` set to
`GLFW_NO_API` (RI, not GLFW, talks to the GPU). RI's own tests and examples use this code;
GLFW is not part of RI.

```cpp
--8<-- "snippets/ri/swapchain.cpp:glfw"
```

Another window library (SDL, Qt, your own Win32 code) fills the same fields from its native
handles.

## Rules / errors

- The handles must stay valid while a swapchain uses them.
- No swapchain exists yet: `RI::Window` is in place so the swapchain milestone adds no new
  portable type.

## Related decisions

[P43](../design-notes/decisions.md#p43) (RI creates no window; a native window handle and a
swapchain), [P109](../design-notes/decisions.md#p109) (no window helper; GLFW only in RI's
tests), [P84](../design-notes/decisions.md#p84) (no native graphics types in portable
headers).
