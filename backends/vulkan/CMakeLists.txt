cmake_minimum_required(VERSION 3.25)

# ── Vulkan ────────────────────────────────────────────────────────────────────
# The loader strategy (Vulkan::Vulkan vs volk) is an open implementation task.
# No allocator here: RI places resources in user memory (CONTEXT.md P31), and
# automatic allocation is a helper (P41, P17).
find_package(Vulkan REQUIRED)

# ── Backend scaffold ──────────────────────────────────────────────────────────
add_library(EquineVulkanBackend STATIC
    VulkanBackendHeaderCheck.cpp
)
target_include_directories(EquineVulkanBackend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(EquineVulkanBackend PUBLIC
    EquineRI
    Vulkan::Vulkan
)
target_compile_features(EquineVulkanBackend PUBLIC cxx_std_20)
