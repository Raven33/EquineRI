#pragma once
#include <concepts>
#include <type_traits>

namespace rvl {
inline namespace utils {
    consteval auto eBit(unsigned b) { return 1u << b; }

    template <typename T>
    concept Bitmask = std::is_enum_v<T> && requires(T value) {
        { enable_bitmask(value) } -> std::convertible_to<bool>;
    };

    #define makeBitmask(E) \
    consteval bool enable_bitmask(E) { return true; }

    namespace bitmask_detail {
        constexpr auto U(Bitmask auto e) {
            return static_cast<std::underlying_type_t<decltype(e)>>(e);
        }
    } // namespace bitmask_detail

    template <Bitmask E>
    constexpr E operator~(E a) noexcept { return E(~bitmask_detail::U(a)); }
    template <Bitmask E>
    constexpr E operator|(E a, E b) noexcept { return E(bitmask_detail::U(a) | bitmask_detail::U(b)); }
    template <Bitmask E>
    constexpr E operator&(E a, E b) noexcept { return E(bitmask_detail::U(a) & bitmask_detail::U(b)); }
    template <Bitmask E>
    constexpr E operator^(E a, E b) noexcept { return E(bitmask_detail::U(a) ^ bitmask_detail::U(b)); }

    template <Bitmask E>
    constexpr E &operator|=(E &a, E b) noexcept { return a = a | b; }
    template <Bitmask E>
    constexpr E &operator&=(E &a, E b) noexcept { return a = a & b; }
    template <Bitmask E>
    constexpr E &operator^=(E &a, E b) noexcept { return a = a ^ b; }

////// Example usage
/*
enum class Permission : uint32_t {
    None = 0, Read = eBit(0), Write = eBit(1), Execute = eBit(2)
};
makeBitmask(Permission)
*/
} // namespace utils
} // namespace rvl
