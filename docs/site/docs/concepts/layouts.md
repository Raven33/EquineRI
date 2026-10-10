# Image layouts are RI's business

## What you never see

On both Vulkan and D3D12, a texture is not just memory: the GPU keeps it in a **layout**, an
arrangement suited to one kind of use (render target, depth test, shader read, copy). Before a
texture is used another way, the program must *transition* it, naming the texture, its old
layout and its new one. In a renderer written directly against either API, this bookkeeping is
a large share of the code and a common source of bugs.

In RI you never name, track or transition a layout
([P16](../design-notes/decisions.md#p16)). No RI function takes a layout, no RI struct has one,
and your code is the same on every backend and every GPU. What you write instead:

- **usage flags** at texture creation ([`TextureUsage`](../reference/texture.md)), which say
  what the texture is for;
- **rendering sections**, clears and discards, which are RI's own operations;
- **barriers** between your own work, which name stages, never textures
  ([`cmdBarrier`](../reference/cmdBarrier.md)).

The rest of this page explains how each backend makes that possible. It is background: you do
not need it to use RI, but it explains the rules you do see (why a discard exists, why barriers
name no texture, what a usage flag costs).

## The idea: every texture rests in one layout

RI's approach is called **A'** in the design notes. Each texture has one **resting layout**,
chosen once from its usage flags. Between operations, every texture is in its resting layout.

- Your **shaders** (compute, vertex, pixel) read and write textures in their resting layout. RI
  cannot see which textures a bindless shader touches, so the resting layout must already allow
  shader access.
- RI's **own operations** (a rendering section, a clear, a discard) know exactly which textures
  they touch. When one needs another layout, RI switches the texture into it at the start and
  back to rest at the end.

So a texture is never left in an unexpected layout, there is no per-texture state to track
across command lists, and every command list starts and ends with all textures at rest.

## D3D12: resting layouts per usage

D3D12 (with enhanced barriers) has a layout that allows shader reads, shader writes and copies
at once on the direct queue: `DIRECT_QUEUE_COMMON`. RI uses it whenever it can:

| Texture usage | Resting layout |
|---|---|
| any of `eSampled`, `eStorage`, `eCopySrc`, `eCopyDst` | `DIRECT_QUEUE_COMMON` |
| `eRenderTarget` only | `RENDER_TARGET` |
| `eDepthStencil` only | `DEPTH_STENCIL_WRITE` |

A render target that shaders also sample rests in `DIRECT_QUEUE_COMMON`: `cmdBeginRendering`
switches it to `RENDER_TARGET` and `cmdEndRendering` switches it back. A target that is only
ever rendered to stays in `RENDER_TARGET` and needs no switch at all.

## Vulkan: three modes

Vulkan has more layouts, and drivers differ in what they cost. The Vulkan backend picks one of
three **layout modes** when the device is created ([P78](../design-notes/decisions.md#p78)).
They change nothing in your code.

| Mode | Used when | Resting layout | Switches |
|---|---|---|---|
| `eUnified` | the driver has `VK_KHR_unified_image_layouts` | `GENERAL` for every texture | none: the extension makes `GENERAL` as fast as the specific layouts |
| `eTransitions` (mode T) | no extension (until measured, see below) | per usage, like D3D12 | around RI's operations |
| `eGeneral` (mode G) | forced for testing or measuring | `GENERAL` for every texture | none, but attachments may lose compression on some GPUs |

In mode T, the resting layouts are:

| Texture usage | Resting layout |
|---|---|
| `eStorage` (with anything else) | `GENERAL` |
| `eSampled` (color or depth) | `READ_ONLY_OPTIMAL` |
| `eRenderTarget` only | `COLOR_ATTACHMENT_OPTIMAL` |
| `eDepthStencil` only | `ATTACHMENT_OPTIMAL` |
| copies only | `GENERAL` |

The mode in use is reported by `RI::Vk::native(device).layoutMode`, and a device-creation
option can force one ([Vulkan: device creation](../native/vulkan/device-creation.md)).

## RI's switches: an exact half and a full half

A layout switch is a barrier, and a barrier has two halves: what it waits for, and what it
blocks. RI knows its own operation, so the half that touches it is **exact**, and the other
half, which touches your unknown work, is **full**
([P68](../design-notes/decisions.md#p68)):

- a switch **into** an RI operation (at `cmdBeginRendering`, before a clear) waits for all
  earlier work and blocks only that operation;
- a switch **back to rest** after it (at `cmdEndRendering`, after a clear) waits only for that
  operation and blocks all later work.

RI writes these switches immediately, where the operation is recorded. It never holds back or
merges your barriers, so there is no hidden state.

When the old contents are not needed (load operation `eClear` or `eDontCare`, a clear of the
whole view), the switch starts from "undefined" and the GPU skips preserving them.

## Your barriers are global

Because textures are always at rest between operations, a barrier between your own commands
never needs to change a layout. It only has to order work and make writes visible, and that is
what a **global** barrier does: it names stages, not resources
([P36](../design-notes/decisions.md#p36), [P72](../design-notes/decisions.md#p72)). See
[Barriers](barriers.md).

Global barriers cover what textures **at rest** allow: shader reads and writes, copies and
clears. Attachment writes are a special case. They happen inside a rendering section, and on
D3D12 a global barrier cannot name render-target or depth accesses. So RI's switch back to rest
at `cmdEndRendering` is what makes a section's attachment writes visible; a target that rests in
its attachment layout is only used by later attachment work, which the GPU orders itself.

Write the barrier anyway, every time:

```cpp
--8<-- "snippets/ri/textures.cpp:barrier-render-sample"
```

On Vulkan in the `eUnified` and `eGeneral` modes there is no switch at `cmdEndRendering`, and
this barrier is what orders the sampling after the rendering. On D3D12 it costs nothing extra.

## What a texture taking over memory needs

A texture placed over memory that another resource used holds garbage, including garbage in its
hidden **compression metadata**, which a load operation does not reset. RI cannot know when
memory changes hands, so you say it: [`cmdDiscard`](../reference/cmdDiscard.md)
([P69](../design-notes/decisions.md#p69)). It switches the texture from "undefined" to its
resting layout, which resets the metadata. See [Textures and views](textures.md#discard-and-aliasing).

## Is this fast enough?

Hiding layouts has a possible cost: on D3D12 and in Vulkan mode T, an attachment that shaders
also read makes a round trip through its resting layout at every section, which can trigger
decompression work. Whether A' is accepted is decided by the **Milestone 3 measurement**,
against hand-written optimal transitions, with a rule fixed before measuring
([P79](../design-notes/decisions.md#p79)): at most 2% slower per frame and 10% per pass on every
tested GPU. The result is pending. If A' fails somewhere, the fallbacks change the backend (the
other Vulkan mode, for example), not your code.

## In native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:rendering"
    ```

=== "Vulkan"

    Every transition is yours, written with `vkCmdPipelineBarrier2`:

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:transition-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:rendering"
    ```

    What to watch for: a descriptor names the layout the image will be in when the shader
    reads it, so the layout at draw time must match the descriptor. Separate depth and stencil
    layouts need `separateDepthStencilLayouts`.

=== "D3D12"

    Every layout change is a texture barrier (enhanced barriers):

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:texture-barrier-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:rendering"
    ```

    What to watch for: the sync and access values must be allowed by the layouts on each
    side, and global barriers cannot change layouts.

Native code that works on RI's textures gets each texture's resting layout from
`RI::Vk::native(texture)` or `RI::D3D12::native(texture)`, and must put the texture back in it
([P27](../design-notes/decisions.md#p27)): see [Vulkan: native handles](../native/vulkan/objects.md)
and [D3D12: native objects](../native/d3d12/objects.md).

## Related decisions

- [P16](../design-notes/decisions.md#p16): users never see layouts.
- [P58](../design-notes/decisions.md#p58): usage flags decide the resting layout.
- [P68](../design-notes/decisions.md#p68): exact and full halves of RI's switches.
- [P69](../design-notes/decisions.md#p69): discard.
- [P36](../design-notes/decisions.md#p36), [P72](../design-notes/decisions.md#p72): global,
  stage-to-stage barriers.
- [P78](../design-notes/decisions.md#p78): the Vulkan modes.
- [P79](../design-notes/decisions.md#p79): the decision rule of the measurement.
- [P27](../design-notes/decisions.md#p27): native code and resting layouts.
