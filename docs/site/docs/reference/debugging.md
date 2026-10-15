---
symbols: [RI::setName, RI::cmdBeginMarker, RI::cmdEndMarker]
---

# RI::setName, RI::cmdBeginMarker, RI::cmdEndMarker

## What it does

Debugging aids that are **always on**, in every build ([P53](../design-notes/decisions.md#p53)):

- **Names at creation** ([P106](../design-notes/decisions.md#p106)): every creation desc has a
  `name` field, its last (`BufferDesc`, `TextureDesc`, `TextureViewDesc`, `ComputePipelineDesc`,
  `GraphicsPipelineDesc`, `WorkGraphDesc`, `DescriptorHeapDesc`, `QueryPoolDesc`,
  `SwapchainDesc`), and `allocateMemory`, `createCommandList` and
  `createAccelerationStructure` take it as their last argument. GPU debuggers and captures
  (RenderDoc, PIX, Nsight) and validation messages show it. RI copies it during the call, so a
  temporary string is fine. An empty name, the default, names nothing. A swapchain's images are
  named `<name> [i]`, also after a resize.
- `RI::setName(device, object, name)` renames an object later. It takes memory, buffers,
  textures, texture views, descriptor heaps, query pools, pipelines and command lists, and runs on
  the [device thread](../concepts/threads.md).
- `RI::cmdBeginMarker(cmd, name)` / `RI::cmdEndMarker(cmd)` open and close a named region of
  commands in a capture. Regions nest.

Names are UTF-8. They cost a driver call each, so name objects once, at creation, and rename
only when the name must change.

## Signature

--8<-- "generated/signatures/RI.setName.md"

--8<-- "generated/signatures/RI.cmdBeginMarker.md"

--8<-- "generated/signatures/RI.cmdEndMarker.md"

## Example

```cpp
--8<-- "snippets/ri/debugging.cpp:names"
```

```cpp
--8<-- "snippets/ri/debugging.cpp:markers"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/debugging.cpp:markers"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/debugging.cpp:names"
    ```

    ```cpp
    --8<-- "snippets/vulkan/debugging.cpp:markers"
    ```

    What to watch for: `VK_EXT_debug_utils` is an instance extension, so its functions come
    from `vkGetInstanceProcAddr`, and the instance must enable it.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/debugging.cpp:names"
    ```

    ```cpp
    --8<-- "snippets/d3d12/debugging.cpp:markers"
    ```

    What to watch for: `SetName` takes UTF-16. Markers are PIX events; without the PIX
    headers, `BeginEvent` with metadata 0 and a UTF-16 string (size in bytes, including the
    terminator) is the format PIX and other tools read.

## How RI does it

- **Vulkan**: `VK_EXT_debug_utils`, enabled on every device's instance.
  `vkSetDebugUtilsObjectNameEXT` for names (a texture's name also goes to its whole-texture
  view), `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT` for markers.
- **D3D12**: `ID3D12Object::SetName` (a pipeline's name also goes to its root signature, and a
  heap's to both its native heaps). Texture views are descriptors on D3D12, not objects, so
  naming a view does nothing. Markers are PIX events (`BeginEvent` / `EndEvent`).

## Rules and errors

- `cmdEndMarker` with no open region is a **programmer error** in validation builds.
- A region still open at `cmdEnd` is a **programmer error** in validation builds:
  `cmdEnd: 1 marker region(s) left open; close each with cmdEndMarker (P53)`.

## Related decisions

- [P53](../design-notes/decisions.md#p53): debugging aids; names and markers always on.
- [P106](../design-notes/decisions.md#p106): names at creation; `setName` on the device thread.
- [Debugging](../concepts/debugging.md): crash diagnostics, shader printf and GPU-assisted
  validation in validation builds.
