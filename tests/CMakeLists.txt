# GPU-free unit tests. These link only EquineRI (no backend, no device) so they
# run in any configuration, including CI machines without a GPU.

add_executable(EquineRITests
    "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ValidationTests.cpp"
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

# ── GPU tests ────────────────────────────────────────────────────────────────
# Need a real GPU and driver; labelled "gpu" so `ctest -LE gpu` skips them.
if (TARGET EquineVulkanBackend)
    add_executable(EquineRIVulkanTests
        "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/DeviceTests.cpp"
    )
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
    )
    # The Windows loader (1.4.321) fails the whole instance with
    # VK_ERROR_OUT_OF_HOST_MEMORY when a VK_ADD_DRIVER_FILES driver cannot be
    # loaded (vulkaninfo too), so the premise holds only on Linux. The message
    # filter it checks is platform-independent.
    if (NOT WIN32)
        list(APPEND EQUINE_VK_TEST_NAMES vk.loader_errors_do_not_abort)
    endif()
    foreach(test_name IN LISTS EQUINE_VK_TEST_NAMES)
        add_test(NAME "${test_name}" COMMAND EquineRIVulkanTests "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES
            LABELS gpu
            # Driver leaks only; see the file.
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
        )
    endforeach()
    if (NOT WIN32)
        # A driver manifest whose library does not exist.
        set(EQUINE_MISSING_VULKAN_DRIVER "equine_missing_vulkan_driver${CMAKE_SHARED_LIBRARY_SUFFIX}")
        configure_file(vulkan/broken_icd.json.in "${CMAKE_CURRENT_BINARY_DIR}/broken_icd.json" @ONLY)
        set_tests_properties(vk.loader_errors_do_not_abort PROPERTIES
            ENVIRONMENT "VK_ADD_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/broken_icd.json;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
        )
    endif()
    set_tests_properties(vk.unknown_gpu_reports PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: the GPU 'Nonexistent GPU' is not available"
    )
    list(LENGTH EQUINE_VK_TEST_NAMES EQUINE_VK_TEST_COUNT)
    add_test(NAME vk.registry_matches COMMAND EquineRIVulkanTests --expect-count ${EQUINE_VK_TEST_COUNT})
endif()

if (TARGET EquineD3D12Backend)
    add_executable(EquineRID3D12Tests
        "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/AgilityExports.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/DeviceTests.cpp"
    )
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
    foreach(test_name IN LISTS EQUINE_D3D12_TEST_NAMES)
        add_test(NAME "${test_name}" COMMAND EquineRID3D12Tests "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES LABELS gpu)
    endforeach()
    set_tests_properties(d3d12.unknown_gpu_reports PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: the GPU 'Nonexistent GPU' is not available"
    )
    list(LENGTH EQUINE_D3D12_TEST_NAMES EQUINE_D3D12_TEST_COUNT)
    add_test(NAME d3d12.registry_matches COMMAND EquineRID3D12Tests --expect-count ${EQUINE_D3D12_TEST_COUNT})
endif()

# ── Backend conformance (P83) ────────────────────────────────────────────────
# The same source, compiled against each backend; building it is the test.
foreach(backend IN ITEMS Vulkan D3D12)
    if (TARGET Equine${backend}Backend)
        add_library(EquineConformance${backend} OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/conformance/Conformance.cpp")
        target_link_libraries(EquineConformance${backend} PRIVATE Equine${backend}Backend)
    endif()
endforeach()
