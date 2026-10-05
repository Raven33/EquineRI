#pragma once

#ifndef EQ_FORCEINLINE
#if defined(_MSC_VER)
#define EQ_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define EQ_FORCEINLINE inline __attribute__((always_inline))
#else
#define EQ_FORCEINLINE inline
#endif
#endif

#include <cassert>
#include <stdexcept>
#ifdef NDEBUG
#define ASSERT(condition, message)             \
    do {                                       \
        if (!(condition)) {                    \
            throw std::runtime_error(message); \
        }                                      \
    } while (false)
#else
#define ASSERT(condition, message) assert((condition) && (message))
#endif
