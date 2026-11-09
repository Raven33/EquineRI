cmake_minimum_required(VERSION 3.25)

# ── Metal (P113) ──────────────────────────────────────────────────────────────
# metal-cpp, pinned in the repository (git submodule): Apple's header-only C++
# wrapper, whose private implementation (the Objective-C selectors and class
# lookups) is compiled once, in MetalCpp.cpp. Metal 4 and Apple silicon only.
equine_require_submodule("${EQUINE_THIRD_PARTY_DIR}/metal-cpp/Metal/Metal.hpp" metal-cpp)
add_library(EquineMetalCpp STATIC MetalCpp.cpp)
target_include_directories(EquineMetalCpp SYSTEM PUBLIC "${EQUINE_THIRD_PARTY_DIR}/metal-cpp")
target_link_libraries(EquineMetalCpp PUBLIC
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
)
equine_mark_nonproject_dependency(EquineMetalCpp)

# ── Backend library (P113) ───────────────────────────────────────────────────
add_library(EquineMetalBackend STATIC
    RI/Metal/Backend.hpp
    RI/Metal/Native.hpp
    DeviceState.hpp
    Device.cpp
    Resources.cpp
    Textures.cpp
    TextureCommands.cpp
    Pipeline.cpp
    Commands.cpp
    Indirect.cpp
    RayTracing.cpp
    Rendering.cpp
    Debug.cpp
    Queries.cpp
    Unsupported.cpp
)
target_include_directories(EquineMetalBackend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EquineMetalBackend PUBLIC EquineRI EquineMetalCpp)
target_compile_features(EquineMetalBackend PUBLIC cxx_std_20)
# Selects this backend for <RI/RI.hpp> in every target that links it (P83).
target_compile_definitions(EquineMetalBackend PUBLIC EQUINE_BACKEND_METAL=1)
