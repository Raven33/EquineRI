---
symbols: [RI::cmdSetRootData]
---

# RI::cmdSetRootData

## What it does

`RI::cmdSetRootData(cmd, value)` sets the current pipeline's [root data](../concepts/root-data.md):
a small struct of values (heap slots, constants) that the shader declares with
`EQ_ROOT_DATA` and that the following dispatches read. The bytes are copied into the command
list at once.

There are two forms: a template that takes any struct and sends `sizeof(value)` bytes, and a
pointer-and-size form for data you build yourself.

## Signature

--8<-- "generated/signatures/RI.cmdSetRootData.md"

## Example

The struct mirrors the shader's declaration:

```hlsl
--8<-- "snippets/hlsl/doc_compute.hlsl:root-data"
```

```cpp
--8<-- "snippets/ri/compute.cpp:root-struct"
```

```cpp
--8<-- "snippets/ri/compute.cpp:set-state"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:set-state"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:root-data"
    ```

    What to watch for: push constants are set with a pipeline layout and stage flags that
    must match the layout's push-constant range. RI's ranges use `VK_SHADER_STAGE_ALL`.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:root-data"
    ```

    What to watch for: root constants are counted in 32-bit values, and parameter 0 is the
    one RI's root signatures reserve for root data.

## Rules and errors

- Call it after [`cmdSetPipeline`](cmdSetPipeline.md): the data belongs to the current
  pipeline. Set it again after switching pipelines.
- In validation builds, the size must equal the shader's (`pipeline.rootDataSize`): otherwise
  `programmer error: root data is 8 bytes, but the pipeline's shader declares 20 bytes`. A
  pipeline whose shader declares no root data rejects any.
- Keep the C++ struct in the same order and layout as the HLSL one; RI's compile rule uses
  D3D12's buffer layout on both backends ([P54](../design-notes/decisions.md#p54)).

## Related decisions

- [P45](../design-notes/decisions.md#p45): `EQ_ROOT_DATA`.
- [P44](../design-notes/decisions.md#p44): the size is the shader's.
- [P24](../design-notes/decisions.md#p24): checked against reflection under validation.
- [P94](../design-notes/decisions.md#p94): root data in Milestone 2.
