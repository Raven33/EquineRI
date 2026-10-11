# Documentation snippets (docs/DocsProposal.md, "Keeping the site in sync").
#
# Every file under ri/, vulkan/ and d3d12/ is a complete program that the site
# shows a marked section of. This file compiles each one as a translation unit
# against the backend it belongs to, so the examples cannot rot. Compile only:
# OBJECT libraries are never linked or run.
#
#   ri/      RI code, compiled once per enabled backend (P83: user code is
#            compiled once per backend).
#   vulkan/  canonical Vulkan and Vulkan native access (<RI/Vk/Native.hpp>),
#            compiled against EquineVulkanBackend.
#   hlsl/    the examples' shaders, compiled to SPIR-V and DXIL with RI's rule.
#   d3d12/   canonical D3D12 and D3D12 native access (<RI/D3D12/Native.hpp>),
#            compiled against EquineD3D12Backend; skipped when that backend is
#            off (always on Linux).
#
# Turned on with -DEQUINE_BUILD_DOC_SNIPPETS=ON; the free CI job never builds it.

file(GLOB EQUINE_DOC_SNIPPETS_RI CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ri/*.cpp")
file(GLOB EQUINE_DOC_SNIPPETS_VULKAN CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/*.cpp")
file(GLOB EQUINE_DOC_SNIPPETS_D3D12 CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/*.cpp")

add_custom_target(EquineDocSnippets)

foreach(backend IN ITEMS Vulkan D3D12)
    if (TARGET Equine${backend}Backend AND EQUINE_DOC_SNIPPETS_RI)
        add_library(EquineDocSnippetsRI${backend} OBJECT ${EQUINE_DOC_SNIPPETS_RI})
        target_link_libraries(EquineDocSnippetsRI${backend} PRIVATE Equine${backend}Backend EquineGlfwWindow)
        add_dependencies(EquineDocSnippets EquineDocSnippetsRI${backend})
        # The examples' shader, with RI's compile rule (P93); defines EQUINE_SHADER_DIR.
        equine_add_shaders(EquineDocSnippetsRI${backend} PROFILE cs_6_6 SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/hlsl/doc_compute.hlsl")
        equine_add_shaders(EquineDocSnippetsRI${backend} PROFILE vs_6_6 SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/hlsl/doc_triangle_vs.hlsl")
        equine_add_shaders(EquineDocSnippetsRI${backend} PROFILE ps_6_6 SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/hlsl/doc_triangle_ps.hlsl")
    endif()
endforeach()

if (TARGET EquineVulkanBackend AND EQUINE_DOC_SNIPPETS_VULKAN)
    add_library(EquineDocSnippetsVulkan OBJECT ${EQUINE_DOC_SNIPPETS_VULKAN})
    target_link_libraries(EquineDocSnippetsVulkan PRIVATE EquineVulkanBackend)
    add_dependencies(EquineDocSnippets EquineDocSnippetsVulkan)
else()
    message(STATUS "Equine doc snippets: Vulkan snippets skipped (EQUINE_VULKAN_BACKEND is off)")
endif()

if (TARGET EquineD3D12Backend AND EQUINE_DOC_SNIPPETS_D3D12)
    add_library(EquineDocSnippetsD3D12 OBJECT ${EQUINE_DOC_SNIPPETS_D3D12})
    target_link_libraries(EquineDocSnippetsD3D12 PRIVATE EquineD3D12Backend)
    target_compile_definitions(EquineDocSnippetsD3D12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(EquineDocSnippets EquineDocSnippetsD3D12)
else()
    message(STATUS "Equine doc snippets: D3D12 snippets skipped (EQUINE_D3D12_BACKEND is off)")
endif()
