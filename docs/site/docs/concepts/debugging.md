# Debugging

RI's debugging aids, from always-on to validation builds only
([P53](../design-notes/decisions.md#p53)).

## Always on: names and markers

Name your objects and group your commands into regions, and GPU debuggers (RenderDoc, PIX,
Nsight) and validation messages show those names instead of raw handles: see
[`RI::setName`, `RI::cmdBeginMarker`](../reference/debugging.md).

```cpp
--8<-- "snippets/ri/debugging.cpp:names"
```

## Validation builds: crash diagnostics

When the GPU crashes (a hang, a page fault from a wrong address or index), the next
submission or wait reports a **device lost** error on the [error channel](error-channel.md).
In a [validation build](validation.md) the message also says where:

- **D3D12**: DRED is on. The message lists each command list that did not finish, with how
  many of its operations completed and the next operation, and the GPU address of a page
  fault with the allocations around it (named with `setName`).
- **Vulkan**: `VK_EXT_device_fault` is on when the GPU has it. The message carries the
  driver's description, the faulting addresses and vendor details.

Name your objects: the reports use the names.

## Validation builds: shader printf (Vulkan)

On Vulkan, `printf` in a shader prints to standard output in validation builds, through the
validation layer. DXIL has no `printf` yet, so guard it for SPIR-V:

```hlsl
#ifdef __spirv__
    printf("pixel %u %u: depth %f", id.x, id.y, depth);
#endif
```

A D3D12 version (a buffer the shader writes, read back and printed) is a planned helper.
Release builds compile the call but print nothing.

## Validation builds: GPU-assisted validation

Bindless indices are not visible to the CPU: an out-of-range slot, a descriptor of the
wrong type, or a buffer address past the end is only seen when the shader runs. Validation
builds turn on the GPU-instrumented checks of both backends: Vulkan **GPU-AV** (validation
layer setting `gpuav_enable`) and D3D12 **GPU-based validation**. Errors go to the error
channel like other validation errors.

They have a cost: shaders are instrumented when pipelines are created, and device creation
takes a few seconds longer on Vulkan. Release builds have none of it.
