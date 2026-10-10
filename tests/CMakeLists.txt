# GPU-free unit tests. These link only EquineRI (no backend, no device) so they
# run in any configuration, including CI machines without a GPU.

add_executable(EquineRITests
    "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ValidationTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DxilTests.cpp"
)
target_include_directories(EquineRITests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EquineRITests PRIVATE EquineRI)
equine_copy_sanitizer_runtime(EquineRITests)

# One CTest entry per test case so failures are reported individually.
set(EQUINE_RI_TEST_NAMES
    error.category_names
    error.report_prints_and_aborts
    validation.tracker_alive_then_destroyed
    validation.tracker_rejects_never_created
    validation.tracker_rejects_other_device
    validation.macro_evaluates_only_when_on
    validation.texture_and_rendering_checks
    dxil.fill_heap_and_root_data
    dxil.root_data_4_bytes
    dxil.root_data_64_bytes
    dxil.root_data_with_arrays
    dxil.declared_bindings
    dxil.sampler_heap_only
    dxil.space999_other_register_is_error
    dxil.space999_second_resource_is_error
    dxil.newer_psv_layout
    dxil.oldest_psv_layout_numthreads_from_metadata
    dxil.empty_input
    dxil.not_a_container
    dxil.truncated_container
    dxil.missing_parts
    dxil.truncated_bitcode
    dxil.corrupted_bitcode
    dxil.corrupted_container
    dxil.garbage_bitcode
)
if (EQUINE_VALIDATION)
    list(APPEND EQUINE_RI_TEST_NAMES validation.macro_reports_failure)
endif()

foreach(test_name IN LISTS EQUINE_RI_TEST_NAMES)
    add_test(NAME "ri.${test_name}" COMMAND EquineRITests "${test_name}")
endforeach()

# Cases that abort by design catch SIGABRT and exit cleanly; they pass only if
# the expected message was printed.
set_tests_properties(ri.error.report_prints_and_aborts PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[EquineRI\\] unsupported: mesh shaders are not available\n  at .*ErrorTests\\.cpp:[0-9]+"
)
if (EQUINE_VALIDATION)
    set_tests_properties(ri.validation.macro_reports_failure PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: slot 3 is still in use\n  at .*ValidationTests\\.cpp:[0-9]+"
    )
endif()

# Guard against a test case existing in source but never registered above.
list(LENGTH EQUINE_RI_TEST_NAMES EQUINE_RI_TEST_COUNT)
add_test(NAME ri.registry_matches COMMAND EquineRITests --expect-count ${EQUINE_RI_TEST_COUNT})


# Portable GPU tests (tests/gpu/), compiled into each backend's GPU test
# executable and registered as "<prefix>.<case>".
set(EQUINE_GPU_SHARED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/ComputeTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/TextureTests.cpp"
)
set(EQUINE_GPU_SHARED_TEST_NAMES
    compute.buffer_compute_readback
    compute.command_list_reuse_and_timeline
    compute.memory_kinds
    texture.create_and_views
    texture.render_then_sample
    texture.clear_load_op
    texture.depth_write_then_sample
    texture.read_only_depth_section
    texture.msaa_render_then_sample
    texture.storage_write_then_sample
    texture.clear_commands
    texture.discard_after_aliasing
    texture.timestamps
    texture.unlisted_view_format_reports
)
set(EQUINE_GPU_SHARED_ABORT_TESTS
    "texture.unlisted_view_format_reports|programmer error: createView: format"
)
if (EQUINE_VALIDATION)
    list(APPEND EQUINE_GPU_SHARED_TEST_NAMES
        compute.root_data_size_mismatch_reports
        compute.pipeline_before_heap_reports
        compute.descriptor_slot_out_of_range_reports
        compute.buffer_used_after_destroy_reports
        texture.depth_write_in_read_only_section_reports
        texture.missing_discard_reports
    )
    list(APPEND EQUINE_GPU_SHARED_ABORT_TESTS
        "compute.root_data_size_mismatch_reports|programmer error: root data is 8 bytes, but the pipeline's shader declares 20 bytes"
        "compute.pipeline_before_heap_reports|programmer error: cmdSetPipeline: set a descriptor heap with cmdSetHeap first"
        "compute.descriptor_slot_out_of_range_reports|programmer error: slot 8 is outside the resource heap"
        "compute.buffer_used_after_destroy_reports|programmer error: this buffer was destroyed"
        "texture.depth_write_in_read_only_section_reports|programmer error: cmdSetPipeline: the pipeline writes depth"
        "texture.missing_discard_reports|programmer error: cmdBeginRendering: the texture was placed over memory"
    )
endif()

# The shaders of the portable GPU tests.
function(equine_add_gpu_test_shaders exe)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    equine_add_shaders(${exe} PROFILE cs_6_6 SOURCES "${dir}/fill.hlsl" "${dir}/read_texture.hlsl" "${dir}/write_storage.hlsl")
    equine_add_shaders(${exe} PROFILE vs_6_6 SOURCES "${dir}/fullscreen_vs.hlsl")
    equine_add_shaders(${exe} PROFILE ps_6_6 SOURCES "${dir}/solid_ps.hlsl")
endfunction()

# equine_add_gpu_tests(<exe> <prefix> <names...>): one CTest entry per case.
function(equine_add_gpu_tests exe prefix)
    set(names ${ARGN} ${EQUINE_GPU_SHARED_TEST_NAMES})
    foreach(name IN LISTS names)
        set(test "${prefix}.${name}")
        if (name MATCHES "^${prefix}\\.")
            set(test "${name}")
        endif()
        add_test(NAME "${test}" COMMAND ${exe} "${name}")
        set_tests_properties("${test}" PROPERTIES
            LABELS gpu
            # Driver leaks only; see the file.
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
        )
    endforeach()
    foreach(entry IN LISTS EQUINE_GPU_SHARED_ABORT_TESTS)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 message)
        string(REPLACE "(" "\\(" message "${message}")
        string(REPLACE ")" "\\)" message "${message}")
        set_tests_properties("${prefix}.${name}" PROPERTIES PASS_REGULAR_EXPRESSION "\\[EquineRI\\] ${message}")
    endforeach()
    list(LENGTH names count)
    add_test(NAME "${prefix}.registry_matches" COMMAND ${exe} --expect-count ${count})
endfunction()

# ── GPU tests ────────────────────────────────────────────────────────────────
# Need a real GPU and driver; labelled "gpu" so `ctest -LE gpu` skips them.
if (TARGET EquineVulkanBackend)
    add_executable(EquineRIVulkanTests
        "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/DeviceTests.cpp"
        ${EQUINE_GPU_SHARED_SOURCES}
    )
    equine_add_gpu_test_shaders(EquineRIVulkanTests)
    target_include_directories(EquineRIVulkanTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(EquineRIVulkanTests PRIVATE EquineVulkanBackend)
    equine_copy_sanitizer_runtime(EquineRIVulkanTests)

    set(EQUINE_VK_TEST_NAMES
        vk.list_gpus
        vk.create_destroy_device
        vk.two_devices
        vk.native_features_turn_on_robustness
        vk.native_extension_and_pnext
        vk.list_gpus_while_device_alive
        vk.unknown_gpu_reports
        vk.layout_modes
    )
    # The Windows loader (1.4.321) fails the whole instance with
    # VK_ERROR_OUT_OF_HOST_MEMORY when a VK_ADD_DRIVER_FILES driver cannot be
    # loaded (vulkaninfo too), so the premise holds only on Linux. The message
    # filter it checks is platform-independent.
    if (NOT WIN32)
        list(APPEND EQUINE_VK_TEST_NAMES vk.loader_errors_do_not_abort)
    endif()
    equine_add_gpu_tests(EquineRIVulkanTests vk ${EQUINE_VK_TEST_NAMES})
    set_tests_properties(vk.unknown_gpu_reports PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: the GPU 'Nonexistent GPU' is not available"
    )
    if (NOT WIN32)
        # A driver manifest whose library does not exist.
        set(EQUINE_MISSING_VULKAN_DRIVER "equine_missing_vulkan_driver${CMAKE_SHARED_LIBRARY_SUFFIX}")
        configure_file(vulkan/broken_icd.json.in "${CMAKE_CURRENT_BINARY_DIR}/broken_icd.json" @ONLY)
        set_tests_properties(vk.loader_errors_do_not_abort PROPERTIES
            ENVIRONMENT "VK_ADD_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/broken_icd.json;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
        )
    endif()
endif()

if (TARGET EquineD3D12Backend)
    add_executable(EquineRID3D12Tests
        "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/AgilityExports.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/DeviceTests.cpp"
        ${EQUINE_GPU_SHARED_SOURCES}
    )
    equine_add_gpu_test_shaders(EquineRID3D12Tests)
    target_include_directories(EquineRID3D12Tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(EquineRID3D12Tests PRIVATE EquineD3D12Backend)
    equine_copy_sanitizer_runtime(EquineRID3D12Tests)
    equine_deploy_agility_sdk(EquineRID3D12Tests)

    set(EQUINE_D3D12_TEST_NAMES
        d3d12.list_gpus
        d3d12.create_destroy_device
        d3d12.agility_sdk_loaded
        d3d12.two_devices
        d3d12.native_hook_runs_before_creation
        d3d12.list_gpus_while_device_alive
        d3d12.unknown_gpu_reports
    )
    equine_add_gpu_tests(EquineRID3D12Tests d3d12 ${EQUINE_D3D12_TEST_NAMES})
    set_tests_properties(d3d12.unknown_gpu_reports PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: the GPU 'Nonexistent GPU' is not available"
    )
endif()

# ── Backend conformance (P83) ────────────────────────────────────────────────
# The same source, compiled against each backend; building it is the test.
foreach(backend IN ITEMS Vulkan D3D12)
    if (TARGET Equine${backend}Backend)
        add_library(EquineConformance${backend} OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/conformance/Conformance.cpp")
        target_link_libraries(EquineConformance${backend} PRIVATE Equine${backend}Backend)
    endif()
endforeach()

# ── Test shaders (P93) ───────────────────────────────────────────────────────
equine_add_shaders(EquineRITests PROFILE cs_6_6 SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fill.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/root4.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/root64.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/root_arrays.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/declared.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sampler_heap.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/misuse_b1.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/misuse_extra.hlsl"
)
