# Native access

RI never blocks you: every Vulkan or D3D12 object behind an RI object is reachable, so a
feature RI does not wrap (a brand-new extension, an experimental D3D12 feature) is always
usable ([P22](../design-notes/decisions.md#p22)).

Native access is **gated** ([P84](../design-notes/decisions.md#p84)). RI's portable headers
contain no Vulkan or D3D12 types. To reach native objects you must:

1. include the backend's native header, `<RI/Vk/Native.hpp>` or `<RI/D3D12/Native.hpp>`;
2. name the backend explicitly: `RI::Vk::native(device)`, not `RI::native(device)`. The
   `RI::` alias does not bring native functions in.

Code that must build on both backends puts each native part in a `#if` block:

```cpp
--8<-- "snippets/ri/native-access.cpp:includes"
```

```cpp
--8<-- "snippets/ri/native-access.cpp:per-backend"
```

Native code for the wrong backend does not compile, so a mistake shows at build time.

## Pages

| Backend | Page | What you can do |
|---|---|---|
| Vulkan | [Device creation](vulkan/device-creation.md) | Change the features RI enables (e.g. robustness), add extensions and feature structures, reach the instance, device, queue and function table. |
| D3D12 | [Device creation](d3d12/device-creation.md) | Run code just before the device is created (e.g. experimental features), reach the factory, adapter, device and queue. |

Native sections inside command lists, and importing natively created objects into RI, will
be documented here when they are implemented
([P27](../design-notes/decisions.md#p27), [P77](../design-notes/decisions.md#p77)).
