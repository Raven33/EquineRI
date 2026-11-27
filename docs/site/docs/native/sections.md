---
symbols: [RI::Vk::beginNative, RI::Vk::endNative, RI::Vk::NativeCommands, RI::Vk::NativeLayoutChange, RI::D3D12::beginNative, RI::D3D12::endNative, RI::D3D12::NativeCommands, RI::D3D12::NativeLayoutChange, RI::Metal::beginNative, RI::Metal::endNative, RI::Metal::NativeCommands]
---

# Native sections

## What they are

A **native section** puts native commands in the middle of an RI command list
([P27](../design-notes/decisions.md#p27), [P70](../design-notes/decisions.md#p70)): an
extension RI does not wrap, a vendor library that records its own commands, a technique you
already have in native code.

- `beginNative(cmd)` returns the native command buffer (Vulkan, with its function table) or
  command list (D3D12). Every texture is in its **resting layout**, which
  `native(texture).restingLayout` reports. On Metal it returns the `MTL4::CommandBuffer`
  outside rendering sections, and the section's `MTL4::RenderCommandEncoder` inside one (see
  [Metal](#metal) below).
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

=== "Metal"

    ```cpp
    const RI::Metal::NativeCommands n = RI::Metal::beginNative(cmd);
    // Outside a rendering section: open and end your own encoders.
    MTL4::ComputeCommandEncoder *encoder = n.commandBuffer->computeCommandEncoder();
    encoder->fillBuffer(RI::Metal::native(buffer), NS::Range(0, 256), 0);
    encoder->endEncoding();
    RI::Metal::endNative(cmd); // no layouts on Metal: nothing to hand back
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
- `endNative` without `beginNative` is a **programmer error**, and on Metal so is `cmdEnd`
  inside a native section.

## Metal

Metal records through **encoders** opened from the command buffer: a render encoder is one
render pass (Vulkan's `vkCmdBeginRendering` to `vkCmdEndRendering`), a compute encoder runs
dispatches, copies and acceleration-structure builds. One encoder is open at a time, and
bindings belong to it ([P118](../design-notes/decisions.md#p118)).

- **Outside a rendering section**, `beginNative` ends RI's encoder and returns the
  `commandBuffer`. Open and end your own encoders (compute or render, and your own indirect
  command buffers with `executeCommandsInBuffer`), and leave none open at `endNative`. RI's
  next encoder starts from RI's state.
- **Inside a rendering section**, it returns the section's `renderEncoder`. Do not end it,
  change attachments or set its visibility result mode (the section's visibility buffer holds
  RI's occlusion queries). `endNative` sets RI's argument table, pipeline state (depth-stencil,
  cull mode, winding, fill mode, depth clip and bias), viewport, scissor and stencil reference
  again. An RI occlusion query open across the section counts the native draws, as on Vulkan.
- Record no RI commands between `beginNative` and `endNative`.
- `endNative(cmd)` takes no list: Metal has no image layouts.
- **Barriers:** `cmdBarrier` works on both sides, as on the other backends: one recorded before
  `beginNative` reaches your encoders, one after `endNative` makes RI's next work wait for
  them. Inside your own encoders, Metal's barriers are yours.
- **Residency:** everything the GPU reads must be in a residency set. Keep your own
  `MTL::ResidencySet` for your objects (an indirect command buffer, its buffers), add it to
  the queue (`native(device).graphicsQueue->addResidencySet`) or to the command buffer, and
  commit it yourself. RI's set (`native(device).residencySet`) is RI's: do not change it.

## Related decisions

- [P27](../design-notes/decisions.md#p27): the in/out map.
- [P70](../design-notes/decisions.md#p70): the restore contract and sections inside rendering.
- [P118](../design-notes/decisions.md#p118): native sections on Metal (what is handed over,
  residency).
