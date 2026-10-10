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
