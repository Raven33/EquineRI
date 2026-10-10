# GPU-free unit tests. These link only EquineRI (no backend, no device) so they
# run in any configuration, including CI machines without a GPU.

add_executable(EquineRITests
    "${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTests.cpp"
)
target_include_directories(EquineRITests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EquineRITests PRIVATE EquineRI)
equine_copy_sanitizer_runtime(EquineRITests)

# One CTest entry per test case so failures are reported individually.
set(EQUINE_RI_TEST_NAMES
    error.category_names
    error.report_prints_and_aborts
)

foreach(test_name IN LISTS EQUINE_RI_TEST_NAMES)
    add_test(NAME "ri.${test_name}" COMMAND EquineRITests "${test_name}")
endforeach()

# Cases that abort by design catch SIGABRT and exit cleanly; they pass only if
# the expected message was printed.
set_tests_properties(ri.error.report_prints_and_aborts PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[EquineRI\\] unsupported: mesh shaders are not available\n  at .*ErrorTests\\.cpp:[0-9]+"
)

# Guard against a test case existing in source but never registered above.
list(LENGTH EQUINE_RI_TEST_NAMES EQUINE_RI_TEST_COUNT)
add_test(NAME ri.registry_matches COMMAND EquineRITests --expect-count ${EQUINE_RI_TEST_COUNT})
