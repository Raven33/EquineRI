#pragma once

#include <types.hpp>

namespace RI {

constexpr static uint64 INVALID_UUID = 0;

template <typename Tag>
struct Handle {
    uint64 id = INVALID_UUID;

    explicit operator bool() const { return id != INVALID_UUID; }
    explicit operator uint64() const { return id; } // make the handle convertible to uint64 but not implicitly convertible to avoid accidental misuse
};

} // namespace RI
