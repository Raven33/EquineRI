# What MTLTextureUsagePixelFormatView costs (Metal backend, P74): native Metal
# through metal-cpp, offscreen. Results in docs/measurements/metal_pixel_format_view/.
if (TARGET EquineMetalCpp)
    add_executable(metal_view_bench "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    target_link_libraries(metal_view_bench PRIVATE EquineMetalCpp)
endif()
