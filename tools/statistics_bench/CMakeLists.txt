# The statistics-cost measurement (P105): one executable per backend (P83).
foreach(backend IN ITEMS Vulkan D3D12)
    if (NOT TARGET Equine${backend}Backend)
        continue()
    endif()
    set(exe "statistics_bench_${backend}")
    add_executable(${exe}
        "${CMAKE_CURRENT_SOURCE_DIR}/Queries.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Queries${backend}.cpp"
    )
    target_include_directories(${exe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${exe} PRIVATE Equine${backend}Backend EquineHelpers${backend})
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    equine_add_shaders(${exe} PROFILE as_6_6 SOURCES "${dir}/stat_as.hlsl")
    equine_add_shaders(${exe} PROFILE ms_6_6 SOURCES "${dir}/stat_ms.hlsl")
    equine_add_shaders(${exe} PROFILE ps_6_6 SOURCES "${dir}/stat_ps.hlsl")
    equine_add_shaders(${exe} PROFILE cs_6_6 SOURCES "${dir}/stat_cs.hlsl")
    equine_copy_sanitizer_runtime(${exe})
    if (backend STREQUAL "D3D12")
        equine_deploy_agility_sdk(${exe})
    endif()
endforeach()
