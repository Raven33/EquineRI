cmake_minimum_required(VERSION 3.25)

if (NOT WIN32)
    message(FATAL_ERROR "The Direct3D 12 backend is supported only on Windows.")
endif()

# ── Agility SDK (pinned download; CONTEXT.md P55) ────────────────────────────
include(EquineAgilitySDK)

# ── Backend scaffold ──────────────────────────────────────────────────────────
add_library(EquineD3D12Backend STATIC
    D3D12AgilityExports.hpp
    D3D12BackendHeaderCheck.cpp
)
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
