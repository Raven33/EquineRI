---
symbols: [RI::setName, RI::cmdBeginMarker, RI::cmdEndMarker]
---

# RI::setName, RI::cmdBeginMarker, RI::cmdEndMarker

## What it does

Debugging aids that are **always on**, in every build ([P53](../design-notes/decisions.md#p53)):

- `RI::setName(device, object, name)` gives an object a name that GPU debuggers and captures
  (RenderDoc, PIX, Nsight) and validation messages show. It takes memory, buffers, textures,
  texture views, descriptor heaps, query pools, pipelines and command lists.
- `RI::cmdBeginMarker(cmd, name)` / `RI::cmdEndMarker(cmd)` open and close a named region of
  commands in a capture. Regions nest.

Names are UTF-8. They cost a driver call each, so name objects once, at creation.

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
- [Debugging](../concepts/debugging.md): crash diagnostics, shader printf and GPU-assisted
  validation in validation builds.
