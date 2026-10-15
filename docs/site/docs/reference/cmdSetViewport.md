---
symbols: [RI::cmdSetViewport, RI::cmdSetScissor, RI::Viewport, RI::Rect]
---

# RI::cmdSetViewport, RI::cmdSetScissor

## What it does

Inside a [rendering section](../concepts/rendering.md), these change where draws land
([P103](../design-notes/decisions.md#p103)):

- `RI::cmdSetViewport(cmd, viewport)` sets the **viewport**: the rectangle of the render
  target that clip space maps to (`x`, `y`, `width`, `height` in pixels from the top-left
  corner), and the depth range it maps to (`minDepth`, `maxDepth`, 0 to 1 by default). A
  full-screen triangle drawn with a viewport over the left half fills the left half.
- `RI::cmdSetScissor(cmd, rect)` sets the **scissor**: pixels outside this rectangle are not
  written. It cuts; it does not stretch.

Both stay until the next call or the end of the section. Every
[`cmdBeginRendering`](cmdBeginRendering.md) sets them back to cover the attachments, so code
that never calls them draws to the whole target. Uses: split-screen, the tiles of a shadow
atlas, UI panels clipped to their rectangle.

Coordinates are the same on both backends: y goes down from the top-left corner of the
target, while clip space keeps y pointing up
([clip space](../concepts/rendering.md#clip-space-points-up-on-both-backends)).

## Signature

--8<-- "generated/signatures/RI.cmdSetViewport.md"

--8<-- "generated/signatures/RI.cmdSetScissor.md"

--8<-- "generated/signatures/RI.Viewport.md"

--8<-- "generated/signatures/RI.Rect.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:viewport"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:viewport"
    ```

=== "Vulkan"

    ```cpp
    // RI flips the viewport so clip space is y-up, as on D3D12.
    const VkViewport viewport{.x = 0.0f, .y = 128.0f, .width = 128.0f, .height = -128.0f,
                              .minDepth = 0.0f, .maxDepth = 1.0f};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor{.offset = {0, 0}, .extent = {128, 128}};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    ```

    What to watch for: viewport and scissor are dynamic state of the pipelines RI creates;
    a negative height starts the viewport at its bottom edge.

=== "D3D12"

    ```cpp
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 1.0f};
    list->RSSetViewports(1, &viewport);
    const D3D12_RECT scissor{0, 0, 128, 128}; // left, top, right, bottom
    list->RSSetScissorRects(1, &scissor);
    ```

    What to watch for: the scissor is given by its edges, not its size, and D3D12 always
    applies it: without one, nothing is drawn.

## Rules and errors

- Inside a rendering section only (**programmer error** in validation builds).
- The viewport's `width` and `height` are above 0, and its depth range lies in 0..1; the
  scissor's `x` and `y` are 0 or more (**programmer error** in validation builds).
- A [native section](../native/sections.md) inside a rendering section may change them; RI
  sets the last values you gave back at `endNative`.

## Related decisions

- [P103](../design-notes/decisions.md#p103): viewport and scissor commands, with the
  attachments as the default.
- [P38](../design-notes/decisions.md#p38): the rest of the state is fixed in the pipeline.
