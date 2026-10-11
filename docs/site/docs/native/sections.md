---
symbols: [RI::Vk::beginNative, RI::Vk::endNative, RI::Vk::NativeCommands, RI::Vk::NativeLayoutChange, RI::D3D12::beginNative, RI::D3D12::endNative, RI::D3D12::NativeCommands, RI::D3D12::NativeLayoutChange]
---

# Native sections

## What they are

A **native section** puts native commands in the middle of an RI command list
([P27](../design-notes/decisions.md#p27), [P70](../design-notes/decisions.md#p70)): an
extension RI does not wrap, a vendor library that records its own commands, a technique you
already have in native code.

- `beginNative(cmd)` returns the native command buffer (Vulkan, with its function table) or
  command list (D3D12). Every texture is in its **resting layout**, which
  `native(texture).restingLayout` reports.
- Record native commands. You may change layouts, bind your own pipelines and descriptor
  sets, push your own constants.
- `endNative(cmd, changed)` ends the section. `changed` lists the textures you left in
  another layout, with the layout they are in: RI switches them back to rest (full barriers on
  both sides). Then RI **restores everything it set** ([P70](../design-notes/decisions.md#p70)):
  the descriptor heaps (Vulkan descriptor sets), root signatures or pipeline layouts, the last
  pipeline, the last root data, and inside a rendering section the viewport and scissor. After
  `endNative`, RI commands run as if the section had not happened.

## Example

=== "Vulkan"

    ```cpp
    const RI::Vk::NativeCommands n = RI::Vk::beginNative(cmd);
    // ... a native barrier from native(texture).restingLayout to TRANSFER_DST_OPTIMAL,
    //     then a native clear ...
    const RI::Vk::NativeLayoutChange left[] = {{texture.view, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}};
    RI::Vk::endNative(cmd, left);
    ```

=== "D3D12"

    ```cpp
    const RI::D3D12::NativeCommands n = RI::D3D12::beginNative(cmd);
    // ... a native texture barrier from native(texture).restingLayout to COPY_DEST,
    //     then CopyTextureRegion ...
    const RI::D3D12::NativeLayoutChange left[] = {{texture.view, D3D12_BARRIER_LAYOUT_COPY_DEST}};
    RI::D3D12::endNative(cmd, left);
    ```

## Rules and errors

- Inside a rendering section ([P70](../design-notes/decisions.md#p70) (2)): native code must
  not end rendering or change attachments, and the `changed` list must be empty (layouts cannot
  change while rendering) (**programmer error** otherwise).
- List every texture you left in another layout. RI cannot see native layout changes; a
  missing entry leaves the texture in the wrong layout for RI's next use, which the native
  validation layers report.
- Synchronization with RI's commands is yours: a native write that RI work reads needs a
  barrier, native or [`cmdBarrier`](../reference/cmdBarrier.md).
- Sections do not nest (**programmer error**).
- RI restores what *RI* set. State RI does not track (for example a native index buffer
  binding on Vulkan) is not restored; set it again with RI if RI draws use it.

## Related decisions

- [P27](../design-notes/decisions.md#p27): the in/out map.
- [P70](../design-notes/decisions.md#p70): the restore contract and sections inside rendering.
