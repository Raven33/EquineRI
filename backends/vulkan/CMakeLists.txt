cmake_minimum_required(VERSION 3.25)

# ── Vulkan ────────────────────────────────────────────────────────────────────
# Only the headers: functions are loaded at runtime by volk (CONTEXT.md P89),
# so nothing links the system loader. The headers are pinned in the repository
# (git submodule, P96): an older installed SDK would silently compile out
# extensions RI uses (VK_KHR_unified_image_layouts with SDK 1.4.309). No
# allocator here: RI places resources in user memory (P31, P41, P17).
equine_require_submodule("${EQUINE_THIRD_PARTY_DIR}/Vulkan-Headers/include/vulkan/vulkan_core.h" Vulkan-Headers)
add_library(EquineVulkanHeaders INTERFACE)
target_include_directories(EquineVulkanHeaders SYSTEM INTERFACE "${EQUINE_THIRD_PARTY_DIR}/Vulkan-Headers/include")

# volk (git submodule, pinned tag): built here from its single source file rather
# than through volk's own CMake. VK_NO_PROTOTYPES is public so no translation
# unit sees the loader's prototypes.
equine_require_submodule("${EQUINE_THIRD_PARTY_DIR}/volk/volk.c" volk)
add_library(volk STATIC
    "${EQUINE_THIRD_PARTY_DIR}/volk/volk.c"
)
target_include_directories(volk PUBLIC
    "${EQUINE_THIRD_PARTY_DIR}/volk"
)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(volk PUBLIC
    EquineVulkanHeaders
    ${CMAKE_DL_LIBS}
)
equine_mark_nonproject_dependency(volk)

# SPIRV-Reflect (git submodule, pinned tag): reads root data size and bindings
# from SPIR-V (P67). Built from its single source file.
equine_require_submodule("${EQUINE_THIRD_PARTY_DIR}/SPIRV-Reflect/spirv_reflect.c" SPIRV-Reflect)
add_library(spirv-reflect STATIC
    "${EQUINE_THIRD_PARTY_DIR}/SPIRV-Reflect/spirv_reflect.c"
)
target_include_directories(spirv-reflect PUBLIC
    "${EQUINE_THIRD_PARTY_DIR}/SPIRV-Reflect"
)
equine_mark_nonproject_dependency(spirv-reflect)

# ── Backend library ──────────────────────────────────────────────────────────
add_library(EquineVulkanBackend STATIC
    RI/Vk/Backend.hpp
    RI/Vk/Native.hpp
    DeviceState.hpp
    Device.cpp
    Instance.hpp
    Instance.cpp
    VkCheck.hpp
    VkCheck.cpp
    Resources.cpp
    Textures.cpp
    Pipeline.cpp
    Commands.cpp
    Debug.cpp
    Indirect.cpp
    Swapchain.cpp
    RayTracing.cpp
    WorkGraph.cpp
)
target_include_directories(EquineVulkanBackend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(EquineVulkanBackend PUBLIC
    EquineRI
    volk
)
target_link_libraries(EquineVulkanBackend PRIVATE
    spirv-reflect
)
target_compile_features(EquineVulkanBackend PUBLIC cxx_std_20)
# Selects this backend for <RI/RI.hpp> in every target that links it (P83).
target_compile_definitions(EquineVulkanBackend PUBLIC EQUINE_BACKEND_VULKAN=1)
