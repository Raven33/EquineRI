# Reference

One page per public function or structure of RI. Every page has the same shape:

1. **What it does**: a plain explanation.
2. **Signature**: generated from the header, so it is always current.
3. **Example**: RI code taken from a source file that compiles.
4. **The same in native code**: tabs with a canonical Vulkan and D3D12 version.
5. **Rules and errors**: what RI checks and which error it reports.
6. **Related decisions**: the design decisions behind it.

## How names work

You write `RI::createDevice`, whatever the backend. Each backend defines its own functions
in its own namespace (`RI::Vk::`, `RI::D3D12::`), and `<RI/RI.hpp>` makes `RI::` name the
backend your target links ([P83](../design-notes/decisions.md#p83),
[P90](../design-notes/decisions.md#p90)). Functions that expose Vulkan or D3D12 types are
not reachable through `RI::`; they are under [Native access](../native/index.md).

RI objects are **plain structs with public fields** and operations are **free functions**
([P81](../design-notes/decisions.md#p81), [P82](../design-notes/decisions.md#p82)).
Descriptions are filled with C++20 designated initializers, and fields you leave out keep
their defaults ([P86](../design-notes/decisions.md#p86)):

```cpp
RI::Device device = RI::createDevice({.gpu = chosen});
```

## GPUs and devices

| Page | What it is for |
|---|---|
| [`RI::listGpus`](listGpus.md) | The GPUs that can run RI. |
| [`RI::createDevice`, `RI::destroyDevice`](createDevice.md) | Open a GPU for use, and close it. |
| [`RI::Gpu`](Gpu.md) | The facts about one GPU: name, vendor, memory, ID. |
| [`RI::DeviceDesc`, `RI::Device`](DeviceDesc.md) | What you pass to `createDevice`, and what you get back. |

What is still missing is listed in the [coverage report](../about/coverage.md).
