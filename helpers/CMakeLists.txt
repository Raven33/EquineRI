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
