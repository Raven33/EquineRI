# Helpers (P41): optional code built only on RI's public surface. Each one can
# be replaced by your own.

# ── GLFW window helper (P43) ──────────────────────────────────────────────────
# RI creates no window; this helper turns a GLFW window into an RI::Window.
# GLFW is a pinned submodule (third_party/glfw, tag 3.4), built here.
equine_require_submodule("${EQUINE_THIRD_PARTY_DIR}/glfw/CMakeLists.txt" glfw)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${EQUINE_THIRD_PARTY_DIR}/glfw" "${CMAKE_CURRENT_BINARY_DIR}/glfw" EXCLUDE_FROM_ALL SYSTEM)
equine_mark_nonproject_dependency(glfw)
# Third-party code: its warnings are not ours.
target_compile_options(glfw PRIVATE $<IF:$<STREQUAL:${CMAKE_C_COMPILER_FRONTEND_VARIANT},MSVC>,/w,-w>)

add_library(EquineGlfwWindow STATIC glfw/GlfwWindow.cpp)
target_include_directories(EquineGlfwWindow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/glfw")
target_link_libraries(EquineGlfwWindow PUBLIC EquineRI glfw)

# ── RI helpers (P41) ───────────────────────────────────────────────────────────
# Built only on RI's public surface, so compiled once per backend (P83), like
# user code: EquineHelpersVulkan, EquineHelpersD3D12.
set(EQUINE_HELPER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/MemoryAllocator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/SlotAllocator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Transfers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/FrameRing.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/Profiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/FrameGraph.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/ShaderCompiler.cpp"
)
foreach(backend IN ITEMS Vulkan D3D12)
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
