# Samples: the same source built once per enabled backend (P83). Reference
# workloads are listed in CONTEXT.md §3.

foreach(backend IN ITEMS Vulkan D3D12)
    if (NOT TARGET Equine${backend}Backend)
        continue()
    endif()
    set(sample "compute_readback_${backend}")
    add_executable(${sample} "${CMAKE_CURRENT_SOURCE_DIR}/compute_readback/main.cpp")
    target_link_libraries(${sample} PRIVATE Equine${backend}Backend)
    equine_add_shaders(${sample} PROFILE cs_6_6 SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/compute_readback/compute_readback.hlsl")
    equine_copy_sanitizer_runtime(${sample})
    if (backend STREQUAL "D3D12")
        equine_deploy_agility_sdk(${sample})
    endif()
    # The Milestone 2 exit: the sample runs and reads back correct values.
    if (EQUINE_BUILD_TESTS)
        add_test(NAME "sample.compute_readback.${backend}" COMMAND ${sample})
        set_tests_properties("sample.compute_readback.${backend}" PROPERTIES
            LABELS gpu
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/vulkan/lsan.supp:print_suppressions=0"
        )
    endif()
endforeach()
