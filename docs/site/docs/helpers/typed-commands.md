# Typed commands

## What it does

Core commands that take raw data take bytes, `std::span<const std::byte>`
([P107](../design-notes/decisions.md#p107)): [`cmdSetRootData`](../reference/cmdSetRootData.md)
and [`cmdWriteSmallData`](../reference/cmdWriteSmallData.md). Their size rules are checked in
validation builds, at run time. `<RI/Helpers/TypedCommands.hpp>` adds forms that take a value
and check those rules **at compile time**, which costs nothing when the program runs:

| Helper | Accepts a type `T` that is |
|---|---|
| `RI::Helpers::cmdSetRootData(cmd, value)` | `RootDataValue`: trivially copyable, `sizeof(T)` a multiple of 4 and at most `kMaxRootDataSize` (256) |
| `RI::Helpers::cmdWriteSmallData(cmd, buffer, offset, value)` | `SmallWriteValue`: a `RootDataValue` of at most `kMaxSmallWriteSize` (64) bytes |

Pointers and spans are refused: they are trivially copyable too, but their bytes are an address,
never the value meant. The span forms are callable through `RI::Helpers` as well, so code with
`using namespace RI::Helpers` can call both.

Each helper is one line: it views the value's bytes with `std::as_bytes(std::span(&value, 1))`
and calls core. That view is the one the C++ standard defines for an object's bytes, so no
cast is involved.

## Example

```cpp
--8<-- "snippets/ri/copies.cpp:small-write"
```

```cpp
RI::Helpers::cmdSetRootData(cmd, Root{.inputSlot = 0, .outputSlot = 1, .count = kCount});
RI::Helpers::cmdWriteSmallData(cmd, flags, 16, std::array<uint32, 4>{1, 0, 0, 1});
// RI::Helpers::cmdWriteSmallData(cmd, flags, 0, std::array<uint32, 17>{}); // does not compile: 68 bytes
```

## What stays a run-time check

- The root data's size against the pipeline's (`pipeline.rootDataSize`): the pipeline is known
  only at run time.
- A small write's `offset` (a multiple of 4) and its range in the buffer.

## How to roll your own

Call core with `std::as_bytes(std::span(&value, 1))`, and put your own constraints on the type.
