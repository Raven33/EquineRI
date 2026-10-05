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

#include <cstdint>
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

#define EQ_USEGLM 1
#ifdef EQ_USEGLM
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>
// Vectors
using vec2f = glm::vec2;
using vec3f = glm::vec3;
using vec4f = glm::vec4;
using vec2i = glm::ivec2;
using vec3i = glm::ivec3;
using vec4i = glm::ivec4;
using vec2u = glm::uvec2;
using vec3u = glm::uvec3;
using vec4u = glm::uvec4;
// Matrices
using mat2f = glm::mat2;
using mat3f = glm::mat3;
using mat4f = glm::mat4;
#endif

#include <chrono>
using nanosecondsF = std::chrono::duration<double, std::nano>;
using microsecondsF = std::chrono::duration<double, std::micro>;
using millisecondsF = std::chrono::duration<double, std::milli>;
