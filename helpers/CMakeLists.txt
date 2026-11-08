# Helpers (P41): optional code built only on RI's public surface. Each one can
# be replaced by your own.

# ── RI helpers (P41) ───────────────────────────────────────────────────────────
# Built only on RI's public surface, so compiled once per backend (P83), like
# user code: EquineHelpersVulkan, EquineHelpersD3D12, EquineHelpersMetal. On
# Metal, ShaderCompiler yields plain DXIL, which the Metal backend refuses: it
# takes .mtl.dxil from the build's Metal Shader Converter step (P114).
set(EQUINE_HELPER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/MemoryAllocator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/SlotAllocator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Transfers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/FrameRing.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Profiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/ShaderCompiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Statistics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Grid.cpp"
)
foreach(backend IN ITEMS Vulkan D3D12 Metal)
    if (TARGET Equine${backend}Backend)
        add_library(EquineHelpers${backend} STATIC ${EQUINE_HELPER_SOURCES})
        target_include_directories(EquineHelpers${backend} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")
        target_link_libraries(EquineHelpers${backend} PUBLIC Equine${backend}Backend PRIVATE ${CMAKE_DL_LIBS})
        # The runtime shader compiler loads the pinned DXC library (P29, P93).
        target_include_directories(EquineHelpers${backend} SYSTEM PRIVATE "${EQUINE_DXC_INCLUDE_DIR}")
        target_compile_definitions(EquineHelpers${backend} PRIVATE
            "EQUINE_DXC_LIBRARY=\"${EQUINE_DXC_LIBRARY}\""
            "EQUINE_SHADER_INCLUDE_DIR=\"${EQUINE_SHADER_INCLUDE_DIR}\"")
    endif()
endforeach()
