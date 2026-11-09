---
symbols: [RI::createSwapchain, RI::destroySwapchain, RI::resizeSwapchain, RI::acquire, RI::present, RI::Swapchain, RI::SwapchainDesc, RI::surfaceFormats, RI::presentModeSupported, RI::screenStorageSupported, RI::SurfaceFormat, RI::ColorSpace, RI::PresentMode]
---

# RI::createSwapchain, RI::acquire, RI::present

## What it does

A **swapchain** shows rendered frames in a window ([P43](../design-notes/decisions.md#p43),
[P63](../design-notes/decisions.md#p63), [P64](../design-notes/decisions.md#p64)). RI creates
no window: you pass its OS handles as an [`RI::Window`](Window.md) (its page shows how to
fill one from a GLFW window).

- `RI::surfaceFormats(device, window)` lists the `(format, color space)` pairs the window can
  show: `ColorSpace::eSrgb`, `eHdr10` (PQ, typically `eRGB10A2Unorm`) or `eScRgb` (linear,
  `eRGBA16Float`). `RI::presentModeSupported(device, window, mode)` says whether a present mode
  is available: `PresentMode::eFifo` (vertical sync, always), `eMailbox` (no tearing, the newest
  frame wins), `eImmediate` (may tear).
- `RI::createSwapchain(device, desc)` creates it for the window's size, format and present
  mode; `imageCount` 0 lets RI choose. `swapchain.width`, `height` and `imageCount` say what was
  made.
- Each frame, `RI::acquire(device, swapchain)` returns the **screen texture**: draw into it like
  any texture (usage `eRenderTarget | eCopyDst`), submit, then `RI::present(queue, swapchain)`.
  There is no copy and no index to track.
- `RI::resizeSwapchain(device, swapchain, width, height)` recreates the images, after waiting
  for the GPU. You decide when: after the window changed size, or after an out-of-date report.
- `RI::destroySwapchain(device, swapchain)` waits for the GPU and destroys it.

**Swapchain images are not frames in flight** ([P43](../design-notes/decisions.md#p43)): how
many frames the CPU records ahead of the GPU is yours to decide with timeline points; one
command list waited on each frame is the simplest correct loop.

## Signature

--8<-- "generated/signatures/RI.createSwapchain.md"

--8<-- "generated/signatures/RI.acquire.md"

--8<-- "generated/signatures/RI.present.md"

--8<-- "generated/signatures/RI.resizeSwapchain.md"

--8<-- "generated/signatures/RI.SwapchainDesc.md"

--8<-- "generated/signatures/RI.surfaceFormats.md"

## Example

```cpp
--8<-- "snippets/ri/swapchain.cpp:create"
```

```cpp
--8<-- "snippets/ri/swapchain.cpp:frame"
```

## How RI does it

- **Vulkan**: a surface for the window system (Win32, Xlib, XCB, Wayland) and a
  `VkSwapchainKHR`. RI hides the semaphores ([P63](../design-notes/decisions.md#p63)):
  `acquire` acquires an image and submits a prerecorded switch that waits on the acquire
  semaphore and moves the image from "undefined" to its resting layout; `present` submits a
  switch to `PRESENT_SRC` that signals the image's present semaphore, then presents. Both
  submissions run in order on the graphics queue, after your work, and count on its
  timeline. The default image count is the surface's minimum plus one.
- **D3D12**: a flip-model DXGI swapchain (`FLIP_DISCARD`, 3 buffers by default). Back
  buffers rest in the `COMMON` layout, which is also `PRESENT`, so RI's switches around
  rendering sections and clears leave them ready to present. `acquire` waits until the
  current back buffer's last frame is done; `present` calls `Present`. Neither submits work
  of its own. sRGB formats use a UNORM
  buffer with sRGB render-target views, as flip model requires. `eFifo` presents with sync
  interval 1, `eMailbox` with 0, `eImmediate` with tearing allowed.
- **Metal**: a `CAMetalLayer` on the window's `NSView`, not framebuffer-only (so copies and,
  except in sRGB formats, compute writes reach the screen texture). `acquire` takes the
  layer's next drawable and makes the queue wait for it before the work submitted after;
  `present` signals the drawable after the work submitted before it, then presents it.
  Neither submits work. 2 or 3 images (3 by default); `eFifo` waits for the display,
  `eImmediate` does not, and there is no `eMailbox`. `eHdr10` is listed when the window's
  screen has HDR headroom.

## Rules and errors

- **The screen texture's contents are undefined after `acquire`**: clear it, or overwrite it
  completely.
- **Compute writes to the screen are not portable** ([P64](../design-notes/decisions.md#p64)):
  `RI::screenStorageSupported(device, window, format)` says whether `desc.storage` (usage
  `eStorage`) is possible. D3D12 never allows it. To write the screen from compute on every
  backend, render into a normal texture and [copy](copies.md) it into the screen texture.
- One `acquire` per `present`; acquiring twice, or presenting without acquiring, is a
  **programmer error**.
- A format, color space or present mode the window does not support is **unsupported**.
- An out-of-date swapchain (the window changed) is reported on the
  [error channel](../concepts/error-channel.md) as `ErrorCategory::eSwapchainOutOfDate`; call
  `resizeSwapchain`.
- D3D12 takes `WindowSystem::eWin32` windows only; Metal takes `WindowSystem::eCocoa` windows only.

## Related decisions

- [P43](../design-notes/decisions.md#p43): windows and swapchains.
- [P63](../design-notes/decisions.md#p63): the D3D12 present model on both backends.
- [P64](../design-notes/decisions.md#p64): formats, color spaces, storage on the screen.
