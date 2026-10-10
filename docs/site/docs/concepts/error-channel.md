---
symbols: [RI::reportError, RI::toString, RI::ErrorCategory]
---

# The error channel

## What it is

Every failure inside RI goes to **one function**, `RI::reportError`, with a **category** that
says what kind of failure it is. What happens next (the *policy*) is decided in one place,
`src/RI/Error.cpp`, and nowhere else ([P57](../design-notes/decisions.md#p57),
[P90](../design-notes/decisions.md#p90)).

Why one channel? The error model is deliberately not decided yet: abort, exceptions, status
codes, or a user callback are all possible, and they may differ per category. Keeping every
failure site on one function means changing the model later touches one file, not every
function in RI.

## The categories

--8<-- "generated/signatures/RI.ErrorCategory.md"

`RI::toString(category)` returns the name printed in messages:

| Category | Printed as | Typical cause |
|---|---|---|
| `eProgrammerError` | `programmer error` | You broke an RI rule: a GPU that `listGpus` did not return, a device destroyed twice, a validation-layer error. |
| `eUnsupported` | `unsupported` | The GPU, driver, or backend lacks something: a Vulkan loader older than 1.3, a missing validation layer in a validation build ([P11](../design-notes/decisions.md#p11)). |
| `eDeviceLost` | `device lost` | The GPU stopped responding or was removed. |
| `eOutOfMemory` | `out of memory` | A GPU or CPU allocation failed. |
| `eSwapchainOutOfDate` | `swapchain out of date` | The window changed and the swapchain must be recreated (no swapchain yet). |

## Today's policy: print and abort

For now **every category prints a message and aborts the program**. The message names the
category, the failure, and the place in RI's source that reported it:

```text
[EquineRI] programmer error: the GPU 'Nonexistent GPU' is not available; pass a GPU returned by listGpus()
  at <RI source file>:<line> (<RI function>)
```

Because the policy never returns, `reportError` is declared `[[noreturn]]`:

--8<-- "generated/signatures/RI.reportError.md"

The location is filled in automatically (`std::source_location`) at the call site in RI,
so it points at the RI function that failed, not at your line. Run under a debugger to see
your own frame in the call stack.

!!! note "Still open"
    The per-category policy is decision X1, left open on purpose: it will be settled by
    using the API. Proposals on record are to keep aborting for programmer errors,
    unsupported features, and device loss, and to return a status for swapchain recreation,
    allocation failure, and pipeline creation (for shader hot reload). Signatures may change
    when that happens.

## Where errors come from

1. **RI's own checks.** For example, `createDevice` with a GPU that is not available, or
   `destroyDevice` on a device that was already destroyed, reports a programmer error.
2. **Native results.** When a Vulkan or D3D12 call fails, RI maps its result code to a
   category:

    | Native result | Category |
    |---|---|
    | `VK_ERROR_OUT_OF_HOST_MEMORY`, `VK_ERROR_OUT_OF_DEVICE_MEMORY`, `VK_ERROR_OUT_OF_POOL_MEMORY`, `VK_ERROR_FRAGMENTED_POOL`, `VK_ERROR_FRAGMENTATION`; `E_OUTOFMEMORY` | out of memory |
    | `VK_ERROR_DEVICE_LOST`; `DXGI_ERROR_DEVICE_REMOVED`, `_HUNG`, `_RESET`, `DXGI_ERROR_DRIVER_INTERNAL_ERROR` | device lost |
    | `VK_ERROR_OUT_OF_DATE_KHR` | swapchain out of date |
    | `VK_ERROR_INITIALIZATION_FAILED`, `_LAYER_NOT_PRESENT`, `_EXTENSION_NOT_PRESENT`, `_FEATURE_NOT_PRESENT`, `_INCOMPATIBLE_DRIVER`, `_FORMAT_NOT_SUPPORTED`; `DXGI_ERROR_UNSUPPORTED`, `DXGI_ERROR_SDK_COMPONENT_MISSING`, `E_NOTIMPL` | unsupported |
    | anything else | programmer error |

    The message names the native call and its result, e.g.
    `vkCreateDevice failed: VK_ERROR_FEATURE_NOT_PRESENT`.

3. **Validation messages.** In validation builds, errors from the Vulkan validation layer
   and the D3D12 debug layer become programmer errors. Warnings are only printed. See
   [Validation](validation.md).

## Using the channel in your own helpers

`reportError` is public. A helper you write on top of RI can report through the same
channel, so one policy covers RI and your code:

```cpp
--8<-- "snippets/ri/errors.cpp:report-error"
```

## How native APIs report errors

Without RI, each API has its own way, and checking every result is up to you.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/errors.cpp:report-error"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/errors.cpp:check-result"
    ```

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/errors.cpp:check-hresult"
    ```

## Related decisions

- [P57](../design-notes/decisions.md#p57): one central channel with categories; the policy
  lives in one place.
- [P90](../design-notes/decisions.md#p90): the channel is `RI::reportError` in
  `src/RI/Error.{hpp,cpp}`; today's policy prints and aborts.
- [P11](../design-notes/decisions.md#p11): a feature missing on one backend or GPU fails
  with a clear error.
- [P91](../design-notes/decisions.md#p91): RI's own validation checks report as programmer
  errors.
