# GPU-written indirect counts on Metal 4: RI's path against indirect command
# buffers (P18, P116 (3)). Native Metal through metal-cpp, offscreen. Results
# in docs/measurements/metal_indirect_count/.
if (TARGET EquineMetalCpp)
    add_executable(indirect_count_bench "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    target_link_libraries(indirect_count_bench PRIVATE EquineMetalCpp)
endif()
