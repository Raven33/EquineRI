# The P112 measurement: a push-descriptor constant buffer against a
# heap-indexed one and root data alone. Raw Vulkan through volk; shaders from
# glslang, so every mode runs the same source.
if (NOT EQUINE_VULKAN_BACKEND)
    return()
endif()
find_program(EQUINE_GLSLANG glslangValidator)
if (NOT EQUINE_GLSLANG)
    message(STATUS "constant_bench: glslangValidator not found, skipped")
    return()
endif()
add_executable(constant_bench "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
target_link_libraries(constant_bench PRIVATE volk)
set(source "${CMAKE_CURRENT_SOURCE_DIR}/bench.comp")
foreach(mode IN ITEMS PUSH HEAP ROOT)
    string(TOLOWER "${mode}" name)
    add_custom_command(TARGET constant_bench POST_BUILD
        COMMAND "${EQUINE_GLSLANG}" -V --target-env vulkan1.3 -S comp "-D${mode}" "${source}"
                -o "$<TARGET_FILE_DIR:constant_bench>/constant_${name}.spv"
        VERBATIM)
endforeach()
