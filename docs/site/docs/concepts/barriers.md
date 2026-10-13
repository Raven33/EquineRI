# Barriers

## Why the GPU needs them

The GPU does not run a command list one command at a time. It overlaps commands whenever it
can, and it keeps writes in caches that other parts of the GPU may not see. So when one
command **writes** memory and a later one **reads** it, you must say so, or the reader may see
old data. That statement is a **barrier**.

In RI, barriers are **explicit**: you write them ([P36](../design-notes/decisions.md#p36)).
RI does not track which buffers a command uses: with bindless, a dispatch does not reveal
which buffers its shader reads, so RI cannot know for certain, and it does not guess.

## Coarse barriers: stage to stage

An RI barrier names two sets of **stages**, not resources
([P94](../design-notes/decisions.md#p94)):

```cpp
--8<-- "snippets/ri/compute.cpp:barrier-copy-compute"
```

`cmdBarrier(cmd, before, after)` means: **every write** made by the `before` stages, in
commands recorded earlier, is finished and visible to **every access** by the `after` stages,
in commands recorded later.

| Stage | Covers |
|---|---|
| `Stage::eCopy` | copy commands (`cmdCopyBuffer`) and clears (`cmdClearTexture`, `cmdClearBuffer`) |
| `Stage::eCompute` | compute shaders (`cmdDispatch`) |
| `Stage::eIndirect` | reading indirect arguments ([`cmdExecuteIndirect`](../reference/indirect.md)) |
| `Stage::eVertex` | vertex shaders (`cmdDraw`), including the buffers they pull vertices from |
| `Stage::ePixel` | pixel shaders |
| `Stage::eRenderTarget` | color attachment reads and writes in a rendering section |
| `Stage::eDepthStencil` | depth and stencil tests and writes in a rendering section |
| `Stage::eAll` | every stage |

Stages combine with `|`.

A barrier is needed **between a write and a later access** of the same memory:

- copy, then a shader reads the copied data: `cmdBarrier(cmd, eCopy, eCompute)`;
- a shader writes, then a copy reads: `cmdBarrier(cmd, eCompute, eCopy)`;
- a shader writes, then another dispatch reads: `cmdBarrier(cmd, eCompute, eCompute)`;
- a rendering section writes a texture, then shaders sample it:
  `cmdBarrier(cmd, eRenderTarget | eDepthStencil, ePixel | eCompute)`;
- a compute shader writes a buffer, then a draw pulls vertices from it:
  `cmdBarrier(cmd, eCompute, eVertex)`.

The `before` side covers everything recorded earlier, **including earlier submissions**: a
command list that reads what a previous submission wrote starts with a barrier too.

Two reads never need a barrier. Two dispatches that write different buffers and read nothing
of each other need none either.

## Why coarse is enough

Naming resources in a barrier ("only this buffer") is the fine-grained form. RI has **no
resource-naming barrier** ([P72](../design-notes/decisions.md#p72)): on desktop GPUs, a
barrier that changes no image layout costs about the same whether it names one resource or
none. Buffers have no layouts, so for them a global barrier is all there is. Textures have layouts,
but RI keeps every texture in one resting layout between operations, so barriers between your
work never change one ([Image layouts are RI's business](layouts.md)). Finer control is
available through native code.

## Attachments

Rendering sections are RI's own operations. Around them RI switches textures between layouts,
and its switch back to rest at `cmdEndRendering` already makes the section's attachment writes
visible on backends that switch. Write the barrier after a section anyway: on Vulkan with
unified layouts there is no switch, and the barrier is what orders the next reads. On D3D12,
global barriers cannot name attachment accesses, so `eRenderTarget` and `eDepthStencil` add
nothing there, and a barrier whose `before` or `after` holds only attachment stages records
nothing.

## The CPU side: nothing to do

On D3D12, once a submission's fence value is reached, the CPU sees its writes in mapped
memory. Vulkan needs one more barrier, from all GPU writes to the **host** stage, before the
CPU can read them. RI's [`cmdEnd`](../reference/cmdBegin.md) adds it on Vulkan, so on both
backends: **after the timeline point, mapped memory holds the GPU's results**. You never write
a barrier to the CPU.

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:record"
    ```

=== "Vulkan"

    A global memory barrier (`VkMemoryBarrier2`) through `vkCmdPipelineBarrier2`:

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:barrier-function"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:record"
    ```

    | RI stage | Vulkan stage |
    |---|---|
    | `eCopy` | `VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT` |
    | `eCompute` | `VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT` |
    | `eIndirect` | `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT` |
    | `eVertex` | `VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT` |
    | `ePixel` | `VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT` |
    | `eRenderTarget` | `VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT` |
    | `eDepthStencil` | `VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT \| LATE_FRAGMENT_TESTS_BIT` |
    | `eAll` | `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` |

    RI uses `MEMORY_WRITE` as the source access and `MEMORY_READ | MEMORY_WRITE` as the
    destination access.

=== "D3D12"

    A global enhanced barrier (`D3D12_GLOBAL_BARRIER`) through `ID3D12GraphicsCommandList7::Barrier`:

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:barrier-function"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:record"
    ```

    | RI stage | D3D12 sync | Accesses |
    |---|---|---|
    | `eCopy` | `SYNC_COPY \| SYNC_CLEAR_UNORDERED_ACCESS_VIEW` | `COPY_SOURCE \| COPY_DEST \| UNORDERED_ACCESS` |
    | `eCompute` | `SYNC_COMPUTE_SHADING` | `UNORDERED_ACCESS \| SHADER_RESOURCE \| CONSTANT_BUFFER` |
    | `eIndirect` | `SYNC_EXECUTE_INDIRECT` | `INDIRECT_ARGUMENT` |
    | `eVertex` | `SYNC_VERTEX_SHADING` | `UNORDERED_ACCESS \| SHADER_RESOURCE \| CONSTANT_BUFFER` |
    | `ePixel` | `SYNC_PIXEL_SHADING` | `UNORDERED_ACCESS \| SHADER_RESOURCE \| CONSTANT_BUFFER` |
    | `eRenderTarget`, `eDepthStencil` | none: covered by RI's switch at `cmdEndRendering` | none |
    | `eAll` | `SYNC_ALL` | every access above |

    What to watch for: enhanced barriers need the Agility SDK and a recent driver; the older
    `ResourceBarrier` with resource states is the legacy model.

## Related decisions

- [P36](../design-notes/decisions.md#p36): explicit barriers, coarse stage to stage by
  default; the inference rule.
- [P72](../design-notes/decisions.md#p72): no resource-naming barrier.
- [P10](../design-notes/decisions.md#p10): coarse is the default form of synchronization.
- [P94](../design-notes/decisions.md#p94): the stages; the host barrier in `cmdEnd` on Vulkan.
