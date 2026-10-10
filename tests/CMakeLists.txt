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
        vk.loader_errors_do_not_abort
        vk.unknown_gpu_reports
    )
    foreach(test_name IN LISTS EQUINE_VK_TEST_NAMES)
        add_test(NAME "${test_name}" COMMAND EquineRIVulkanTests "${test_name}")
        set_tests_properties("${test_name}" PROPERTIES
            LABELS gpu
            # Driver leaks only; see the file.
            ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
        )
    endforeach()
    set_tests_properties(vk.loader_errors_do_not_abort PROPERTIES
        ENVIRONMENT "VK_ADD_DRIVER_FILES=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/broken_icd.json;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lsan.supp:print_suppressions=0"
    )
    set_tests_properties(vk.unknown_gpu_reports PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[EquineRI\\] programmer error: the GPU 'Nonexistent GPU' is not available"
    )
    list(LENGTH EQUINE_VK_TEST_NAMES EQUINE_VK_TEST_COUNT)
    add_test(NAME vk.registry_matches COMMAND EquineRIVulkanTests --expect-count ${EQUINE_VK_TEST_COUNT})
endif()
