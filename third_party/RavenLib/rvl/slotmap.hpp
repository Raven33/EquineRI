#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <concepts>

#include "rvl.hpp"

namespace rvl{
inline namespace containers {
    template <typename T>
    concept SlotMapValue = std::semiregular<T>;

    // KeyTag/KeyTagMask: optional two-bit resource kind tag embedded in the
    // two lowest bits of the 32-bit index field. Tagged maps shift the pool
    // index left by 2 (capacity 2^30) and reject any lookup whose tag bits do
    // not match — a key minted by one tagged map can never resolve against
    // another tagged map, even if their slot states coincide.
    template <SlotMapValue T, bool EnableBoundsCheck = false, std::uint64_t KeyTag = 0, std::uint64_t KeyTagMask = 0>
    class SlotMap {
    private:
        static_assert(KeyTagMask == 0 || KeyTagMask == 0x3ull,
                      "SlotMap key tags use the two lowest bits of the 32-bit index field");
        static_assert((KeyTag & ~KeyTagMask) == 0, "SlotMap key tag must lie within its mask");
        static constexpr std::uint32_t kActiveMask = 1u;

        std::vector<T> pool;
        std::vector<std::uint32_t> meta;
        std::vector<std::uint32_t> free_indices;

        [[nodiscard]] static RVL_FORCEINLINE bool key_tag_matches(std::uint64_t key) noexcept {
            if constexpr (KeyTagMask != 0) {
                return (key & KeyTagMask) == KeyTag;
            }
            return true;
        }

        [[nodiscard]] static RVL_FORCEINLINE std::uint32_t index_from_key(std::uint64_t key) noexcept {
            const std::uint32_t low = static_cast<std::uint32_t>(key & 0xFFFFFFFFull);
            if constexpr (KeyTagMask != 0) {
                return low >> 2;
            }
            return low;
        }

        [[nodiscard]] static RVL_FORCEINLINE std::uint32_t meta_from_key(std::uint64_t key) noexcept {
            return static_cast<std::uint32_t>(key >> 32);
        }

        [[nodiscard]] static RVL_FORCEINLINE std::uint64_t make_key(std::uint32_t index,
                                                                        std::uint32_t slot_meta) noexcept {
            const std::uint32_t shifted = (KeyTagMask != 0) ? (index << 2) : index;
            return (static_cast<std::uint64_t>(slot_meta) << 32) | shifted | KeyTag;
        }

        [[nodiscard]] RVL_FORCEINLINE bool in_bounds(std::uint32_t index) const noexcept {
            if constexpr (EnableBoundsCheck) {
                return index < pool.size() && index < meta.size();
            }
            return true;
        }

        void reset_slot(std::uint32_t index) {
            pool[index] = T{};
        }

        template <typename U>
        std::uint64_t insert_impl(U&& obj) {
            if (!free_indices.empty()) [[unlikely]] {
                const std::uint32_t index = free_indices.back();

                pool[index] = std::forward<U>(obj);
                free_indices.pop_back();
                const std::uint32_t slot_meta = meta[index] | kActiveMask;
                meta[index] = slot_meta;
                return make_key(index, slot_meta);
            }

            const std::uint32_t index = static_cast<std::uint32_t>(pool.size());
            meta.push_back(kActiveMask);
            try {
                pool.push_back(std::forward<U>(obj));
            } catch (...) {
                meta.pop_back();
                throw;
            }
            return make_key(index, kActiveMask);
        }

    public:
        struct CopyResult {
            bool is_valid;
            T value;
        };

        explicit SlotMap(std::uint32_t initial_size) {
            pool.reserve(initial_size);
            meta.reserve(initial_size);
            free_indices.reserve(initial_size);
        }

        [[nodiscard]] std::size_t pool_size() const noexcept {
            return pool.size();
        }

        std::uint64_t insert(const T& obj) {
            return insert_impl(obj);
        }

        std::uint64_t insert(T&& obj) {
            return insert_impl(std::move(obj));
        }

        template <typename... Args>
            requires std::constructible_from<T, Args...>
        std::uint64_t emplace(Args&&... args) {
            return insert_impl(T(std::forward<Args>(args)...));
        }

        RVL_FORCEINLINE void erase(std::uint64_t key) {
            if (!key_tag_matches(key)) [[unlikely]] {
                return;
            }

            const std::uint32_t index = index_from_key(key);
            const std::uint32_t expected_meta = meta_from_key(key);

            if ((expected_meta & kActiveMask) == 0) [[unlikely]] {
                return;
            }

            if constexpr (EnableBoundsCheck) {
                if (!in_bounds(index)) [[unlikely]] {
                    return;
                }
            }

            if (meta[index] == expected_meta) [[likely]] {
                if (free_indices.size() == free_indices.capacity()) {
                    free_indices.reserve(free_indices.size() + 1);
                }
                reset_slot(index);
                meta[index] += 1;
                free_indices.push_back(index);
            }
        }

        [[nodiscard]] RVL_FORCEINLINE CopyResult get_copy(std::uint64_t key) const {
            if (!key_tag_matches(key)) [[unlikely]] {
                return { false, {} };
            }

            const std::uint32_t index = index_from_key(key);
            const std::uint32_t expected_meta = meta_from_key(key);

            if ((expected_meta & kActiveMask) == 0) [[unlikely]] {
                return { false, {} };
            }

            if constexpr (EnableBoundsCheck) {
                if (!in_bounds(index)) [[unlikely]] {
                    return { false, {} };
                }
            }

            if (meta[index] != expected_meta) [[unlikely]] {
                return { false, {} };
            }
            return { true, pool[index] };
        }

        [[nodiscard]] RVL_FORCEINLINE T* getTransient(std::uint64_t key) noexcept {
            if (!key_tag_matches(key)) [[unlikely]] {
                return nullptr;
            }

            const std::uint32_t index = index_from_key(key);
            const std::uint32_t expected_meta = meta_from_key(key);

            if ((expected_meta & kActiveMask) == 0) [[unlikely]] {
                return nullptr;
            }

            if constexpr (EnableBoundsCheck) {
                if (!in_bounds(index)) [[unlikely]] {
                    return nullptr;
                }
            }

            if (meta[index] != expected_meta) [[unlikely]] {
                return nullptr;
            }
            return &pool[index];
        }

        [[nodiscard]] RVL_FORCEINLINE const T* getTransient(std::uint64_t key) const noexcept {
            if (!key_tag_matches(key)) [[unlikely]] {
                return nullptr;
            }

            const std::uint32_t index = index_from_key(key);
            const std::uint32_t expected_meta = meta_from_key(key);

            if ((expected_meta & kActiveMask) == 0) [[unlikely]] {
                return nullptr;
            }

            if constexpr (EnableBoundsCheck) {
                if (!in_bounds(index)) [[unlikely]] {
                    return nullptr;
                }
            }

            if (meta[index] != expected_meta) [[unlikely]] {
                return nullptr;
            }
            return &pool[index];
        }
    };

} // namespace containers
} // namespace rvl
