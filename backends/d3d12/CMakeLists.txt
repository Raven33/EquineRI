cmake_minimum_required(VERSION 3.25)

if (NOT WIN32)
    message(FATAL_ERROR "The Direct3D 12 backend is supported only on Windows.")
endif()

# ── Agility SDK (pinned download; CONTEXT.md P55) ────────────────────────────
include(EquineAgilitySDK)

# ── Backend library ──────────────────────────────────────────────────────────
add_library(EquineD3D12Backend STATIC
    RI/D3D12/Backend.hpp
    RI/D3D12/Native.hpp
    D3D12AgilityExports.hpp
    Adapter.hpp
    Adapter.cpp
    D3D12Check.hpp
    D3D12Check.cpp
    Device.cpp
    Debug.cpp
    DeviceState.hpp
    Resources.cpp
    Pipeline.cpp
    Commands.cpp
    Barriers.hpp
    Formats.hpp
    Formats.cpp
    Textures.cpp
    Rendering.cpp
)
target_compile_definitions(EquineD3D12Backend PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(EquineD3D12Backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(EquineD3D12Backend PUBLIC
    EquineRI
    EquineAgilitySDK
    d3d12
    dxgi
    dxguid
)
target_compile_features(EquineD3D12Backend PUBLIC cxx_std_20)
# Selects this backend for <RI/RI.hpp> in every target that links it (P83).
target_compile_definitions(EquineD3D12Backend PUBLIC EQUINE_BACKEND_D3D12=1)
