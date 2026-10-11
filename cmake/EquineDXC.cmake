# DXC for build-time shader compilation (CONTEXT.md P4, P93)
#
# One pinned DirectX Shader Compiler release, downloaded at configure time and
# verified by SHA-256, like the Agility SDK (P55). The release ships the DXIL
# signer (dxil.dll / libdxil.so), so DXIL compiled on any platform loads on
# D3D12. Used by tests and samples; RI itself consumes bytecode only (P29).
#
# Provides:
#   EQUINE_DXC_EXECUTABLE                          - the dxc executable
#   equine_add_shaders(<target> PROFILE <p> SOURCES <files...>)
#       compiles each file to <build>/shaders/<name>.spv and <name>.dxil, adds
#       them as dependencies of <target>, and defines EQUINE_SHADER_DIR on it.

include_guard(GLOBAL)

set(EQUINE_DXC_VERSION "v1.9.2609")
if (WIN32)
    set(EQUINE_DXC_ARCHIVE "dxc_2026_09_29.zip")
    set(EQUINE_DXC_SHA256 "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
    set(EQUINE_DXC_RELATIVE_EXE "bin/x64/dxc.exe")
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(EQUINE_DXC_ARCHIVE "linux_dxc_2026_09_28.x86_x64.tar.gz")
    set(EQUINE_DXC_SHA256 "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")
    set(EQUINE_DXC_RELATIVE_EXE "bin/dxc")
else()
    message(FATAL_ERROR "No pinned DXC release for this host (${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_PROCESSOR}).")
endif()
set(EQUINE_DXC_URL
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${EQUINE_DXC_VERSION}/${EQUINE_DXC_ARCHIVE}")
set(EQUINE_DXC_ROOT "${CMAKE_BINARY_DIR}/_deps/dxc-${EQUINE_DXC_VERSION}")
set(EQUINE_DXC_EXECUTABLE "${EQUINE_DXC_ROOT}/${EQUINE_DXC_RELATIVE_EXE}")

if (NOT EXISTS "${EQUINE_DXC_EXECUTABLE}")
    message(STATUS "Equine DXC: downloading ${EQUINE_DXC_VERSION}")
    set(EQUINE_DXC_PACKAGE "${EQUINE_DXC_ROOT}-${EQUINE_DXC_ARCHIVE}")
    file(DOWNLOAD "${EQUINE_DXC_URL}" "${EQUINE_DXC_PACKAGE}"
        EXPECTED_HASH SHA256=${EQUINE_DXC_SHA256}
        TLS_VERIFY ON
        STATUS EQUINE_DXC_DOWNLOAD_STATUS
    )
    list(GET EQUINE_DXC_DOWNLOAD_STATUS 0 EQUINE_DXC_DOWNLOAD_CODE)
    if (NOT EQUINE_DXC_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download DXC ${EQUINE_DXC_VERSION}: ${EQUINE_DXC_DOWNLOAD_STATUS}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${EQUINE_DXC_PACKAGE}" DESTINATION "${EQUINE_DXC_ROOT}")
    file(REMOVE "${EQUINE_DXC_PACKAGE}")
    if (NOT EXISTS "${EQUINE_DXC_EXECUTABLE}")
        message(FATAL_ERROR "DXC extraction is missing '${EQUINE_DXC_EXECUTABLE}'. Delete '${EQUINE_DXC_ROOT}' and reconfigure.")
    endif()
endif()
message(STATUS "Equine DXC: ${EQUINE_DXC_VERSION} at ${EQUINE_DXC_EXECUTABLE}")

set(EQUINE_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../shaders")

# One rule for both backends (P54): DX buffer layout on SPIR-V too, so one C++
# mirror of root data fits both. Reflection is stripped from DXIL to prove RI
# does not need it (P67).
function(equine_add_shaders target)
    cmake_parse_arguments(ARG "" "PROFILE" "SOURCES" ${ARGN})
    if (NOT ARG_PROFILE OR NOT ARG_SOURCES)
        message(FATAL_ERROR "equine_add_shaders(<target> PROFILE <profile> SOURCES <files...>)")
    endif()
    set(out_dir "${CMAKE_BINARY_DIR}/shaders")
    foreach(source IN LISTS ARG_SOURCES)
        get_filename_component(source "${source}" ABSOLUTE)
        get_filename_component(name "${source}" NAME_WE)
        # One rule per shader, shared by every target that uses it.
        set(shader_target "equine_shader_${name}")
        if (NOT TARGET "${shader_target}" AND ARG_PROFILE MATCHES "^lib_")
            # Libraries (work graphs, P50) are D3D12 only: DXIL, no entry point.
            add_custom_command(
                OUTPUT "${out_dir}/${name}.dxil"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
                COMMAND "${EQUINE_DXC_EXECUTABLE}" -T ${ARG_PROFILE} -HV 2021 -I "${EQUINE_SHADER_INCLUDE_DIR}" -WX
                        -Qstrip_reflect -Qstrip_debug -Fo "${out_dir}/${name}.dxil" "${source}"
                DEPENDS "${source}" "${EQUINE_SHADER_INCLUDE_DIR}/equine/root.hlsli"
                COMMENT "Compiling ${name} (${ARG_PROFILE}) to DXIL"
                VERBATIM
            )
            add_custom_target("${shader_target}" DEPENDS "${out_dir}/${name}.dxil")
        elseif (NOT TARGET "${shader_target}")
            set(common -T ${ARG_PROFILE} -E main -HV 2021 -I "${EQUINE_SHADER_INCLUDE_DIR}" -WX)
            # Samplers in their own set, so heaps of any size share one layout (P94).
            set(spirv_rules -fvk-bind-sampler-heap 0 1)
            add_custom_command(
                OUTPUT "${out_dir}/${name}.spv" "${out_dir}/${name}.dxil"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
                COMMAND "${EQUINE_DXC_EXECUTABLE}" ${common} -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
                        ${spirv_rules} -Fo "${out_dir}/${name}.spv" "${source}"
                COMMAND "${EQUINE_DXC_EXECUTABLE}" ${common} -Qstrip_reflect -Qstrip_debug
                        -Fo "${out_dir}/${name}.dxil" "${source}"
                DEPENDS "${source}" "${EQUINE_SHADER_INCLUDE_DIR}/equine/root.hlsli"
                COMMENT "Compiling ${name} (${ARG_PROFILE}) to SPIR-V and DXIL"
                VERBATIM
            )
            add_custom_target("${shader_target}" DEPENDS "${out_dir}/${name}.spv" "${out_dir}/${name}.dxil")
        endif()
        add_dependencies("${target}" "${shader_target}")
    endforeach()
    target_compile_definitions("${target}" PRIVATE "EQUINE_SHADER_DIR=\"${out_dir}\"")
endfunction()
