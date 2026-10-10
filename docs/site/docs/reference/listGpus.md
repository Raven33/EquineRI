---
symbols: [RI::listGpus]
---

# RI::listGpus

## What it does

`RI::listGpus()` returns the GPUs in this computer that **can run RI**, as a list of plain
[`RI::Gpu`](Gpu.md) descriptions: name, vendor, kind (discrete, integrated, software),
PCI IDs, memory size, and a stable `id`.

You call it first, pick one GPU from the list, and pass it to
[`RI::createDevice`](createDevice.md). RI does not choose for you: picking "the best" GPU
automatically is a helper's job, not RI's ([P60](../design-notes/decisions.md#p60)).

Step by step, `listGpus`:

1. opens the native API for a moment (Vulkan: a short-lived instance with no layers and no
   extensions; D3D12: a DXGI factory);
2. asks for every GPU, in the order the driver (Vulkan) or DXGI (D3D12) gives them;
3. keeps only the GPUs that meet RI's requirements (below), and describes each one;
4. closes everything it opened. Nothing stays alive between calls.

There is no "instance" object to create first: listing needs nothing from you
([P92](../design-notes/decisions.md#p92)).

## Signature

--8<-- "generated/signatures/RI.listGpus.md"

## Example

```cpp
--8<-- "snippets/ri/device.cpp:list-gpus"
```

Picking a discrete GPU when there is one:

```cpp
--8<-- "snippets/ri/device.cpp:pick-gpu"
```

## The same in native code

The Vulkan and D3D12 code is a *canonical* native version: how you would list GPUs in your
own renderer, not necessarily what RI does internally.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/device.cpp:list-gpus"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:create-instance"
    ```

    ```cpp
    --8<-- "snippets/vulkan/device.cpp:enumerate-gpus"
    ```

    What to watch for:

    - In Vulkan you need an instance before you can see any GPU.
    - `vkEnumeratePhysicalDevices` returns *every* device, including software ones
      (llvmpipe) and GPUs too old for your program: check `apiVersion` and the queue
      families yourself.
    - Most Vulkan queries use the two-call pattern: first the count, then the data.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:create-factory"
    ```

    ```cpp
    --8<-- "snippets/d3d12/device.cpp:enumerate-gpus"
    ```

    What to watch for:

    - DXGI lists adapters, not D3D12 devices. To know what an adapter supports, create a
      short-lived device on it and call `CheckFeatureSupport`.
    - The list includes WARP (the "Microsoft Basic Render Driver", a CPU implementation).
    - Release the probe device before creating your real one (`ComPtr` does it for you).

## Requirements: which GPUs are listed

A GPU is listed only if RI can run on it. Each backend checks with its own API:

- **Vulkan**: the GPU reports Vulkan 1.3 or newer, and has a queue family that does both
  graphics and compute.
- **D3D12**: a device can be created at feature level 12_0, and it supports shader model
  6.6, resource binding tier 3 (full bindless), and enhanced barriers.

Differences you may see between backends:

- D3D12 also lists **WARP** (`GpuKind::eSoftware`). Vulkan lists software drivers such as
  llvmpipe only if they report Vulkan 1.3.
- DXGI may list one physical GPU **twice**, under two different LUIDs (seen with an RTX 3080
  on Windows).
- `memory` of an integrated GPU is computed differently (see [`RI::Gpu`](Gpu.md)).

The hardware floor of the project is NVIDIA Turing and AMD RDNA2
([P3](../design-notes/decisions.md#p3)); older GPUs may pass the checks but are not targets.

## Rules and errors

- An **empty list** is not an error: it means no GPU meets the requirements. Check before
  indexing `gpus[0]`.
- Vulkan: no Vulkan loader on the system, or a loader older than 1.3, reports an
  **unsupported** error.
- A failing native call reports through the [error channel](../concepts/error-channel.md)
  with the category of its result.
- You may call `listGpus` at any time, also while devices exist.
- Validation layers are never loaded for listing, even in validation builds.

## Related decisions

- [P60](../design-notes/decisions.md#p60): RI lists GPUs and you pick one; automatic
  picking is a helper.
- [P92](../design-notes/decisions.md#p92): no instance object; a short-lived instance;
  which GPUs are listed; the D3D12 checks.
- [P2](../design-notes/decisions.md#p2), [P3](../design-notes/decisions.md#p3): backends,
  platforms, and the hardware floor.
