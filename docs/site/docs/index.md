# EquineRI

EquineRI is a **research rendering interface** over Vulkan and Direct3D 12, written in
C++20, with HLSL as the shading language. Its purpose is research velocity: sending work to
the GPU should be trivial, while you keep roughly 90% of the control a native API gives you
([P1](design-notes/decisions.md#p1)).

!!! info "Status"
    The design phase is finished and the implementation has started (Milestone 2 of the
    plan). Today RI runs compute work on both backends: GPUs and devices, memory and placed
    buffers, bindless descriptor heaps, compute pipelines with root data, command lists,
    barriers, and submission with timeline points. Start with
    [your first compute dispatch](getting-started/first-compute.md). Each feature gets its page
    here as it is implemented: a feature is not done until its page exists.

## How this site is organised

1. **[Getting started](getting-started/build.md)**: build the library and run the tests.
2. **Concepts**: the ideas that cut across functions, such as
   [the error channel](concepts/error-channel.md) and [validation](concepts/validation.md).
3. **[Reference](reference/index.md)**: one page per public function or structure.
4. **[Native access](native/index.md)**: how to reach the Vulkan or D3D12 objects behind RI.
5. **[Helpers](helpers/index.md)**: optional conveniences built on RI, each replaceable by
   your own.
6. **[Design notes](design-notes/index.md)**: the decision log. Pages cite decisions by
   number (P60, P92, ...).

## Also a teaching resource

Most reference pages show the RI code next to a **canonical native version** in Vulkan and
in D3D12: how you would do the same thing in your own renderer. If you learned on EquineRI
and want to write your own Vulkan or D3D12 toy renderer, open the page for the RI feature you
know and switch the tabs.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:create-device"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-device"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:create-device"
    ```

The tabs are linked: pick **Vulkan** once and every tab group on the site switches to Vulkan.
The native code is not necessarily what RI does internally; it is the plain way to do it
with the native API alone.

## Using the site

- **Theme**: the sun and moon icon in the header switches between light, dark, and your
  system's setting.
- **Language**: the globe icon switches language. Pages that are not translated yet are shown
  in English.
- **Version**: once versions are published, a selector next to the title switches between
  `latest` (the last release) and `dev` (the development branch).
- **Offline**: the built site works without a server. Open `index.html` from the build
  folder directly in your browser.
