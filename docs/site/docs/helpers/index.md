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
- [Shader compiler](shader-compiler.md): HLSL at run time with RI's compile rule.

Every helper except the GLFW window is in the library `EquineHelpersVulkan` or
`EquineHelpersD3D12`, matching the backend you build.

A frame graph is not a helper: it decides how a whole frame is organised, which belongs to the
layer you build on top of RI ([P35](../design-notes/decisions.md#p35)). RI gives it what it needs:
stage barriers, placed memory, discard.
