# The L1 measurement harness (Milestone 3): one executable per backend (P83).
foreach(backend IN ITEMS Vulkan D3D12)
    if (NOT TARGET Equine${backend}Backend)
        continue()
    endif()
    set(exe "layout_bench_${backend}")
    add_executable(${exe}
        "${CMAKE_CURRENT_SOURCE_DIR}/Bench.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Baseline${backend}.cpp"
    )
    target_include_directories(${exe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${exe} PRIVATE Equine${backend}Backend)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    equine_add_shaders(${exe} PROFILE vs_6_6 SOURCES "${dir}/bench_vs.hlsl" "${dir}/bench_post_vs.hlsl")
    equine_add_shaders(${exe} PROFILE ps_6_6 SOURCES "${dir}/bench_ps.hlsl" "${dir}/bench_post_ps.hlsl")
    equine_add_shaders(${exe} PROFILE cs_6_6 SOURCES
        "${dir}/bench_read.hlsl" "${dir}/bench_write.hlsl" "${dir}/bench_light.hlsl" "${dir}/bench_hiz.hlsl")
    equine_copy_sanitizer_runtime(${exe})
    if (backend STREQUAL "D3D12")
        equine_deploy_agility_sdk(${exe})
    endif()
endforeach()
