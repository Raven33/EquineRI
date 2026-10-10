---
symbols: [RI::cmdDiscard]
---

# RI::cmdDiscard

## What it does

`RI::cmdDiscard(cmd, view)` declares that the texture's contents in `view` are no longer
needed, and **resets** them: texels undefined, hidden compression metadata valid again
([P69](../design-notes/decisions.md#p69)).

It is **required** before the first use of a texture whose memory previously held another
resource (aliasing: see [Textures and views](../concepts/textures.md#discard-and-aliasing)).
Garbage in the compression metadata can corrupt rendering, and a load operation (`eClear`,
`eDontCare`) does not reset it. You may also discard a texture whose contents you no longer
need, before rendering to it with `eDontCare`.

A discard is a **full barrier** for the view: it waits for all earlier work and blocks all
later work. Record it once, right before the first use, outside rendering sections.

## Signature

--8<-- "generated/signatures/RI.cmdDiscard.md"

## Example

```cpp
--8<-- "snippets/ri/textures.cpp:create-aliased"
```

```cpp
--8<-- "snippets/ri/textures.cpp:discard"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/textures.cpp:discard"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/textures.cpp:discard"
    ```

    What to watch for: a transition **from `UNDEFINED`** is the discard: the driver may drop
    contents and reinitialize metadata. A transition from the real old layout would preserve
    the garbage.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/textures.cpp:discard"
    ```

    What to watch for: with enhanced barriers, `LAYOUT_UNDEFINED` plus
    `D3D12_TEXTURE_BARRIER_FLAG_DISCARD`. Aliased render targets and depth textures **must**
    be initialized this way (or by a clear or copy) before use, or rendering may be corrupt on
    some GPUs.

## Rules and errors

- In validation builds, RI tracks placements: a texture placed over bytes another resource
  (alive or destroyed) occupied and then used as an attachment or cleared without a discard is
  a **programmer error**: `cmdBeginRendering: the texture was placed over memory that held
  another resource; record cmdDiscard before its first use (P69; load operations do not
  count)`. Uses by shaders cannot be seen and are not checked; release builds check nothing.
- Not inside a rendering section (**programmer error**).
- Buffers need no discard.
- A new texture in fresh memory needs none: `createTexture` resets it.
- The view's range is discarded: discard the whole texture (`texture.view`) when it takes over
  memory. Discarding part of a texture is for later.

## Related decisions

- [P69](../design-notes/decisions.md#p69): discard, required after aliasing; validation.
- [P35](../design-notes/decisions.md#p35): what a frame graph needs from RI, discard included.
- [P68](../design-notes/decisions.md#p68): RI's layout switches; a discard is full on both
  halves.
