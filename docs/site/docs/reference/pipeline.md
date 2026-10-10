---
symbols: [RI::createComputePipeline, RI::destroyPipeline, RI::Pipeline, RI::ComputePipelineDesc, RI::Bytecode]
---

# RI::createComputePipeline, RI::destroyPipeline

## What it does

A **pipeline** is a shader compiled by the driver for this GPU, with everything it needs to
run. `RI::createComputePipeline(device, desc)` takes the compute shader's **bytecode** (SPIR-V
on Vulkan, DXIL on D3D12; see [Compiling shaders](../concepts/shaders.md)) and returns a
ready `RI::Pipeline` ([P38](../design-notes/decisions.md#p38)). Graphics pipelines (vertex and
pixel shaders) are created with [`createGraphicsPipeline`](graphicsPipeline.md) and destroyed
with the same `destroyPipeline`.

Step by step:

1. RI **reads the shader**: it checks the entry point `main`, that it is a compute shader, and
   that resources are reached through the descriptor heap; it finds the size of the shader's
   [root data](../concepts/root-data.md) ([P45](../design-notes/decisions.md#p45),
   [P67](../design-notes/decisions.md#p67));
2. it builds the **layout** from that: the device's heap sets plus a push-constant range on
   Vulkan, a root signature with root constants and heap indexing on D3D12. You never write a
   layout or a root signature;
3. the driver **compiles** the pipeline, now: creating pipelines can take milliseconds, so do
   it at a moment you choose, never while recording ([P38](../design-notes/decisions.md#p38)).

`pipeline.rootDataSize` holds the root data size read from the shader, in bytes (0 if it
declares none).

## Signature

--8<-- "generated/signatures/RI.createComputePipeline.md"

--8<-- "generated/signatures/RI.destroyPipeline.md"

--8<-- "generated/signatures/RI.Pipeline.md"

--8<-- "generated/signatures/RI.ComputePipelineDesc.md"

--8<-- "generated/signatures/RI.Bytecode.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:load-shader"
```

```cpp
--8<-- "snippets/ri/compute.cpp:create-pipeline"
```

The bytecode is only read during the call; you may free it afterwards.

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:create-pipeline"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:create-pipeline"
    ```

    What to watch for: the pipeline layout must match the shader exactly: its descriptor
    sets and its push-constant size. RI reads both from the SPIR-V with SPIRV-Reflect. The
    shader module can be destroyed once the pipeline exists.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:create-pipeline"
    ```

    What to watch for: bindless shaders need a root signature with the
    `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED` flags. RI finds
    the root data (at `b0, space999`) with its own DXIL reader, which works on stripped
    shaders.

## Rules and errors

**Programmer errors**, in every build, with a readable message:

- the bytecode is not SPIR-V (Vulkan) or not DXIL (D3D12), or is empty;
- no entry point `main`, or not a compute shader;
- more than one root data block;
- a resource declared at a register or binding (`declared resources are not supported yet;
  reach resources through ResourceDescriptorHeap[]`, [P30](../design-notes/decisions.md#p30));
- Vulkan: samplers at set 0, binding 1 (compile with `-fvk-bind-sampler-heap 0 1`).

**Unsupported** error: root data larger than `device.limits.maxRootDataSize`
([P44](../design-notes/decisions.md#p44)).

Destroy a pipeline only when no in-flight GPU work uses it. `destroyPipeline` resets the
struct.

## Related decisions

- [P38](../design-notes/decisions.md#p38): complete state up front; compiled at creation.
- [P23](../design-notes/decisions.md#p23), [P54](../design-notes/decisions.md#p54): the layout
  comes from the shader.
- [P44](../design-notes/decisions.md#p44), [P45](../design-notes/decisions.md#p45): root data
  size from the shader.
- [P67](../design-notes/decisions.md#p67): RI's own DXIL reader.
- [P29](../design-notes/decisions.md#p29): bytecode only.
