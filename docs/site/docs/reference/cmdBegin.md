---
symbols: [RI::cmdBegin, RI::cmdEnd]
---

# RI::cmdBegin, RI::cmdEnd

## What it does

`RI::cmdBegin(cmd)` opens a [command list](commandList.md) for recording; `RI::cmdEnd(cmd)`
closes it. Every `cmd*` call goes between the two, and only a closed list can be
[submitted](submit.md).

`cmdBegin` also forgets the list's previous state: no heap and no pipeline are set, so set
them again in each recording.

`cmdEnd` does one more thing on Vulkan: it records a barrier from all GPU writes to the **host**
(the CPU), so that once the submission's [timeline point](../concepts/timeline.md) is reached,
mapped memory shows the GPU's writes, as it does on D3D12 without a barrier
([P94](../design-notes/decisions.md#p94)). You never write a barrier to the CPU yourself.

## Signature

--8<-- "generated/signatures/RI.cmdBegin.md"

--8<-- "generated/signatures/RI.cmdEnd.md"

## Example

```cpp
--8<-- "snippets/ri/compute.cpp:record"
```

## The same in native code

=== "RI"

    ```cpp
    --8<-- "snippets/ri/compute.cpp:record"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:cmd-begin"
    ```

    ```cpp
    --8<-- "snippets/vulkan/compute.cpp:cmd-end"
    ```

    What to watch for: `ONE_TIME_SUBMIT` tells the driver the buffer is submitted once
    before being reset, which RI's lists always are. Without the host barrier, CPU reads of
    mapped memory after the semaphore wait are not guaranteed to see the GPU's writes.

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:cmd-begin"
    ```

    ```cpp
    --8<-- "snippets/d3d12/compute.cpp:cmd-end"
    ```

    What to watch for: D3D12 has no "begin": `Reset` on the list, with its allocator, opens
    it; a new list is created closed. `Close` ends it.

## Rules and errors

- Begin only a list that is not being executed: reset it first if it was submitted (see
  [`resetCommandList`](commandList.md)).
- Record commands only between `cmdBegin` and `cmdEnd`; submit only after `cmdEnd`.
- A failing native call (e.g. out of memory while closing) reports through the
  [error channel](../concepts/error-channel.md).

## Related decisions

- [P34](../design-notes/decisions.md#p34): immediate recording.
- [P85](../design-notes/decisions.md#p85): recording functions carry the `cmd` prefix.
- [P94](../design-notes/decisions.md#p94): the host barrier in `cmdEnd` on Vulkan.
