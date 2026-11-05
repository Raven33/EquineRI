#pragma once

// Typed forms of the commands that take bytes (P107): the size and alignment
// rules core checks at run time, in validation builds, become compile-time
// constraints here. The root data size against the pipeline stays a run-time
// check.

#include <RI/Abi.hpp>
#include <RI/RI.hpp>

#include <span>
#include <type_traits>

namespace RI::inline EQUINE_RI_ABI::Helpers {

// A pointer or a span is trivially copyable too, but it is never the value meant.
template <typename T>
inline constexpr bool kIsSpan = false;
template <typename T, size_t N>
inline constexpr bool kIsSpan<std::span<T, N>> = true;

template <typename T>
concept RootDataValue = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !kIsSpan<T> &&
                        sizeof(T) % 4 == 0 && sizeof(T) <= kMaxRootDataSize;

template <typename T>
concept SmallWriteValue = RootDataValue<T> && sizeof(T) <= kMaxSmallWriteSize;

// The span forms, callable through RI::Helpers too.
using RI::cmdSetRootData;
using RI::cmdWriteSmallData;

template <RootDataValue T>
void cmdSetRootData(CommandList &cmd, const T &data) {
    RI::cmdSetRootData(cmd, std::as_bytes(std::span(&data, 1)));
}

template <SmallWriteValue T>
void cmdWriteSmallData(CommandList &cmd, const Buffer &buffer, uint64 offset, const T &value) {
    RI::cmdWriteSmallData(cmd, buffer, offset, std::as_bytes(std::span(&value, 1)));
}

} // namespace RI::Helpers
