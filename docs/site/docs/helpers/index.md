# Helpers

RI exposes the explicit, controllable form of every mechanism. The automatic form (picking
a GPU, allocating memory for you, assigning descriptor slots, frames in flight, a profiler,
...) is a **helper**: optional code built only on RI's public functions
([P41](../design-notes/decisions.md#p41), [P15](../design-notes/decisions.md#p15)).

Because a helper uses nothing private, you can always replace it with your own. Each helper
page will therefore end with **"How to roll your own"**: the RI calls the helper makes, so you
can write a version that fits your project.

Helpers:

- [GLFW window](glfw-window.md): the `RI::Window` of a GLFW window, for swapchains.
- [Memory and slot allocators](allocators.md): placement in blocks, slots, deferred frees.
- [Transfers](transfers.md): one-call uploads and readbacks.
- [Frame ring](frame-ring.md): frames in flight.
- [GPU profiler](profiler.md): nested timestamp scopes.
- [Pipeline statistics](statistics.md): a statistics query's counters in one struct.
- [Shader compiler](shader-compiler.md): HLSL at run time with RI's compile rule.
- [Group grids](grid.md): large dispatches folded within 65,535 groups per dimension.
- [Typed commands](typed-commands.md): root data and small writes from a value, with the size checked at compile time.

The frame ring, the memory allocator and the profiler keep a pointer to the `Device` they
were created with: keep the device at the same address, and alive, until you destroy them.

Every helper except the GLFW window is in the library `EquineHelpersVulkan` or
`EquineHelpersD3D12`, matching the backend you build.

A frame graph is not a helper: it decides how a whole frame is organised, which belongs to the
layer you build on top of RI ([P35](../design-notes/decisions.md#p35)). RI gives it what it needs:
stage barriers, placed memory, discard.
