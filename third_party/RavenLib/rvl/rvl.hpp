#pragma once
#if defined(_MSC_VER)
#define RVL_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define RVL_FORCEINLINE inline __attribute__((always_inline))
#else
#define RVL_FORCEINLINE inline
#endif