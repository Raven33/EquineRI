cmake_minimum_required(VERSION 3.25)
project(EquineRI LANGUAGES C CXX)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Module path ───────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

message(STATUS "Equine generator: ${CMAKE_GENERATOR}")
message(STATUS
    "Equine C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)

# ── Compiler runtime policy ──────────────────────────────────────────────────
function(equine_strip_conflicting_compiler_flags)
    foreach(EQUINE_FLAGS_VARIABLE IN ITEMS
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
    )
        if (DEFINED ${EQUINE_FLAGS_VARIABLE})
            set(EQUINE_FLAGS "${${EQUINE_FLAGS_VARIABLE}}")
            string(REGEX REPLACE "(^|[ \t])[-/]fno-exceptions([ \t]|$)" " " EQUINE_FLAGS "${EQUINE_FLAGS}")
            string(REGEX REPLACE "(^|[ \t])[-/]EH[A-Za-z]*-([ \t]|$)" " " EQUINE_FLAGS "${EQUINE_FLAGS}")
            string(REGEX REPLACE "(^|[ \t])[-/]RTC[1csu]*([ \t]|$)" " " EQUINE_FLAGS "${EQUINE_FLAGS}")
            string(REGEX REPLACE "[ \t]+" " " EQUINE_FLAGS "${EQUINE_FLAGS}")
            string(STRIP "${EQUINE_FLAGS}" EQUINE_FLAGS)
            set(${EQUINE_FLAGS_VARIABLE} "${EQUINE_FLAGS}" CACHE STRING "" FORCE)
        endif()
    endforeach()
endfunction()

equine_strip_conflicting_compiler_flags()

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)
endif()

message(STATUS "Equine C++ exceptions: enabled")
# Sanitizers are on by default in every configuration. Turning them off is for
# optimized measurement builds only (the benchmark presets; see
# CMakePresets.json); sanitizer configurations remain the correctness
# coverage.
option(EQUINE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)

if (NOT EQUINE_SANITIZERS)
    message(STATUS "Equine sanitizers: disabled (EQUINE_SANITIZERS=OFF)")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
    message(STATUS "Equine sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
    add_compile_options(/fsanitize=address /clang:-fsanitize=undefined)

    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
        OUTPUT_VARIABLE EQUINE_CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )

    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Equine clang-cl sanitizer runtime wiring currently expects a 64-bit toolchain.")
    endif()

    set(EQUINE_CLANG_RUNTIME_ARCH "x86_64")
    set(EQUINE_CLANG_WINDOWS_RUNTIME_DIR "${EQUINE_CLANG_RESOURCE_DIR}/lib/windows")
    set(EQUINE_CLANG_ASAN_DYNAMIC_LIB "${EQUINE_CLANG_WINDOWS_RUNTIME_DIR}/clang_rt.asan_dynamic-${EQUINE_CLANG_RUNTIME_ARCH}.lib")
    set(EQUINE_CLANG_ASAN_DYNAMIC_THUNK_LIB "${EQUINE_CLANG_WINDOWS_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-${EQUINE_CLANG_RUNTIME_ARCH}.lib")
    set(EQUINE_CLANG_ASAN_DLL "${EQUINE_CLANG_WINDOWS_RUNTIME_DIR}/clang_rt.asan_dynamic-${EQUINE_CLANG_RUNTIME_ARCH}.dll")
    set(EQUINE_CLANG_UBSAN_LIB "${EQUINE_CLANG_WINDOWS_RUNTIME_DIR}/clang_rt.ubsan_standalone-${EQUINE_CLANG_RUNTIME_ARCH}.lib")
    set(EQUINE_CLANG_UBSAN_CXX_LIB "${EQUINE_CLANG_WINDOWS_RUNTIME_DIR}/clang_rt.ubsan_standalone_cxx-${EQUINE_CLANG_RUNTIME_ARCH}.lib")

    foreach(EQUINE_CLANG_RUNTIME_LIB IN ITEMS
        EQUINE_CLANG_ASAN_DYNAMIC_LIB
        EQUINE_CLANG_ASAN_DYNAMIC_THUNK_LIB
        EQUINE_CLANG_ASAN_DLL
        EQUINE_CLANG_UBSAN_LIB
        EQUINE_CLANG_UBSAN_CXX_LIB
    )
        if (NOT EXISTS "${${EQUINE_CLANG_RUNTIME_LIB}}")
            message(FATAL_ERROR "Missing clang-cl sanitizer runtime: ${${EQUINE_CLANG_RUNTIME_LIB}}")
        endif()
    endforeach()

    add_link_options(
        "${EQUINE_CLANG_ASAN_DYNAMIC_LIB}"
        "/INCLUDE:__asan_seh_interceptor"
        "/WHOLEARCHIVE:${EQUINE_CLANG_ASAN_DYNAMIC_THUNK_LIB}"
        "${EQUINE_CLANG_UBSAN_LIB}"
        "${EQUINE_CLANG_UBSAN_CXX_LIB}"
    )
elseif(MSVC)
    message(FATAL_ERROR
        "ASan+UBSan are enabled but MSVC cl.exe does not support UBSan; configure with clang-cl "
        "or set EQUINE_SANITIZERS=OFF."
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(STATUS "Equine sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
else()
    message(FATAL_ERROR
        "ASan+UBSan are required, but compiler '${CMAKE_CXX_COMPILER_ID}' is not configured for them."
    )
endif()

function(equine_copy_sanitizer_runtime target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot attach sanitizer runtime copy step to missing target '${target}'.")
    endif()

    if (WIN32 AND EQUINE_CLANG_ASAN_DLL)
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${EQUINE_CLANG_ASAN_DLL}"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying clang AddressSanitizer DLL to ${target} output directory"
        )
    endif()
endfunction()

# ── Third-party dependencies (git submodules) ────────────────────────────────
include(FetchContent)
set(EQUINE_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

function(equine_require_submodule path name)
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR
            "Missing ${name} submodule at '${path}'. Run 'git submodule update --init --recursive' and reconfigure."
        )
    endif()
endfunction()

function(equine_mark_nonproject_dependency target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR "Expected third-party target '${target}' to exist.")
    endif()

    set_target_properties("${target}" PROPERTIES
        SYSTEM ON
        EXPORT_COMPILE_COMMANDS OFF
        C_CLANG_TIDY ""
        CXX_CLANG_TIDY ""
    )
endfunction()

# RavenLib (header-only, vendored): bitmask operators for RI flag enums (P86).
add_library(rvl INTERFACE)
target_include_directories(rvl INTERFACE
    "${EQUINE_THIRD_PARTY_DIR}/RavenLib"
)

# ── Tooling / diagnostics ─────────────────────────────────────────────────────
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" EQUINE_SOURCE_DIR_FOR_TOOLS)
find_program(EQUINE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
if (MSVC)
    set(EQUINE_CLANG_TIDY_EXCEPTION_ARG /EHsc)
else()
    set(EQUINE_CLANG_TIDY_EXCEPTION_ARG -fexceptions)
endif()
# clang on Windows joins include directories and header names with '\', so every
# path separator in the filter must accept both '/' and '\'.
set(EQUINE_TIDY_SEP "[/\\\\]")
string(REPLACE "/" "${EQUINE_TIDY_SEP}" EQUINE_TIDY_SOURCE_DIR "${EQUINE_SOURCE_DIR_FOR_TOOLS}")
set(CMAKE_CXX_CLANG_TIDY
    "${EQUINE_CLANG_TIDY_EXECUTABLE};--header-filter=^${EQUINE_TIDY_SOURCE_DIR}${EQUINE_TIDY_SEP}(src|backends|third_party${EQUINE_TIDY_SEP}RavenLib)${EQUINE_TIDY_SEP};--extra-arg=${EQUINE_CLANG_TIDY_EXCEPTION_ARG}"
)
message(STATUS "Equine clang-tidy: ${CMAKE_CXX_CLANG_TIDY}")

if (MSVC)
    add_compile_options(/W4)
endif()

# ── Core library ──────────────────────────────────────────────────────────────
# Backend-independent part: portable types shared by every backend (P83) and the
# central error channel (P57). User code includes <RI/RI.hpp>.
add_library(EquineRI STATIC
    src/types.hpp
    src/RI/RI.hpp
    src/RI/Error.hpp
    src/RI/Error.cpp
)
target_include_directories(EquineRI PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(EquineRI PUBLIC
    rvl
)
target_compile_features(EquineRI PUBLIC cxx_std_20)

# Compile-time validation switch (CONTEXT.md P21): native validation layers plus
# Equine's own checks. Each preset sets it explicitly.
option(EQUINE_VALIDATION "Enable native validation layers and Equine's own checks" OFF)
target_compile_definitions(EquineRI PUBLIC "EQUINE_VALIDATION=$<BOOL:${EQUINE_VALIDATION}>")
message(STATUS "Equine validation: ${EQUINE_VALIDATION}")

# ── Optional build features ───────────────────────────────────────────────────
# Each backend is its own library with direct calls and no runtime dispatch
# (CONTEXT.md P28). Both may be built in one tree.
option(EQUINE_BUILD_SAMPLES "Build sample applications" OFF)
option(EQUINE_BUILD_TESTS "Build GPU-free unit tests and register them with CTest" OFF)
option(EQUINE_VULKAN_BACKEND "Build the Vulkan rendering backend" OFF)
option(EQUINE_D3D12_BACKEND "Build the Direct3D 12 rendering backend" OFF)

if (EQUINE_D3D12_BACKEND AND NOT WIN32)
    message(FATAL_ERROR "EQUINE_D3D12_BACKEND is supported only on Windows.")
endif()

# ── HLSL compiler (helper only) ───────────────────────────────────────────────
# The base consumes SPIR-V/DXIL bytecode only; compilation is a helper concern
# (CONTEXT.md P29). One SPIR-V-enabled DXC serves both targets. No target uses it
# yet: the dynamic-compilation helper will.
option(EQUINE_BUILD_SHADER_COMPILER "Locate DXC for the shader-compilation helper" OFF)
if (EQUINE_BUILD_SHADER_COMPILER)
    find_package(DXC REQUIRED)
    if (NOT DXC_SPIRV_FOUND)
        message(FATAL_ERROR
            "The selected DXC does not support SPIR-V code generation: ${DXC_EXECUTABLE}\n"
            "${DXC_SPIRV_ERROR}\n"
            "All backends must use one SPIR-V-enabled DXC installation. "
            "The Vulkan SDK ships one; set DXC_ROOT if needed."
        )
    endif()
endif()

if (EQUINE_VULKAN_BACKEND)
    add_subdirectory(backends/vulkan)
endif()

if (EQUINE_D3D12_BACKEND)
    add_subdirectory(backends/d3d12)
endif()

if (EQUINE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if (EQUINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
