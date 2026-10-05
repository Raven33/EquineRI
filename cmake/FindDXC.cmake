# FindDxc.cmake
# Locates one coherent DirectX Shader Compiler installation and verifies that
# its executable has SPIR-V code generation enabled.
#
# Imported targets:
#   DXC::dxcapi      - dxcapi.h include path
#   DXC::dxcompiler  - selected shared library (used directly on non-Windows)
#
# Result variables:
#   DXC_INCLUDE_DIR
#   DXC_LIBRARY
#   DXC_EXECUTABLE
#   DXC_DLL           - absolute dxcompiler.dll path (Windows)
#   DXC_SPIRV_FOUND   - true only when `dxc -spirv` succeeds
#   DXIL_DLL          - Windows SDK validator/signing runtime, when available

include(FindPackageHandleStandardArgs)

set(DXC_ROOT "" CACHE PATH "Root of a DXC installation (for example, the Vulkan SDK)")

set(_dxc_root_hints)
if (DXC_ROOT)
    file(TO_CMAKE_PATH "${DXC_ROOT}" _dxc_requested_root)
    list(APPEND _dxc_root_hints "${_dxc_requested_root}")
endif()
if (DEFINED ENV{VULKAN_SDK})
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _dxc_vulkan_sdk_root)
    list(APPEND _dxc_root_hints "${_dxc_vulkan_sdk_root}")
endif()
list(REMOVE_DUPLICATES _dxc_root_hints)

set(_dxc_executable_hints)
foreach(_root IN LISTS _dxc_root_hints)
    list(APPEND _dxc_executable_hints
        "${_root}/Bin" "${_root}/Bin/x64"
        "${_root}/bin" "${_root}/bin/x64"
    )
endforeach()

find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS ${_dxc_executable_hints}
    DOC "DXC compiler executable"
)

set(_dxc_install_root "")
set(_dxc_windows_kit_version "")
if (DXC_EXECUTABLE)
    file(TO_CMAKE_PATH "${DXC_EXECUTABLE}" _dxc_executable_normalized)
    get_filename_component(_dxc_bin_dir "${_dxc_executable_normalized}" DIRECTORY)

    foreach(_root IN LISTS _dxc_root_hints)
        string(FIND "${_dxc_executable_normalized}" "${_root}/" _dxc_root_position)
        if (_dxc_root_position EQUAL 0)
            set(_dxc_install_root "${_root}")
            break()
        endif()
    endforeach()

    if (_dxc_executable_normalized MATCHES
        "^(.*Windows Kits/10)/[Bb]in/([^/]+)/x64/dxc\\.exe$")
        set(_dxc_install_root "${CMAKE_MATCH_1}")
        set(_dxc_windows_kit_version "${CMAKE_MATCH_2}")
    endif()

    if (NOT _dxc_install_root)
        get_filename_component(_dxc_install_root "${_dxc_bin_dir}" DIRECTORY)
    endif()
endif()

# The executable is the authoritative installation choice. Discard stale or
# explicitly mixed cache entries rather than combining its runtime with headers
# or libraries from another DXC root.
if (WIN32 AND _dxc_install_root)
    foreach(_dxc_cached_path_var IN ITEMS DXC_INCLUDE_DIR DXC_LIBRARY)
        if (DEFINED ${_dxc_cached_path_var})
            file(TO_CMAKE_PATH "${${_dxc_cached_path_var}}" _dxc_cached_path)
            string(FIND "${_dxc_cached_path}" "${_dxc_install_root}/" _dxc_cached_root_position)
            if (NOT _dxc_cached_root_position EQUAL 0)
                unset(${_dxc_cached_path_var} CACHE)
                unset(${_dxc_cached_path_var})
            endif()
        endif()
    endforeach()
endif()
if (WIN32 AND DEFINED DXC_DLL AND _dxc_bin_dir)
    file(TO_CMAKE_PATH "${DXC_DLL}" _dxc_cached_dll)
    get_filename_component(_dxc_cached_dll_dir "${_dxc_cached_dll}" DIRECTORY)
    if (NOT "${_dxc_cached_dll_dir}" STREQUAL "${_dxc_bin_dir}")
        unset(DXC_DLL CACHE)
        unset(DXC_DLL)
    endif()
endif()

set(_dxc_include_hints)
set(_dxc_library_hints)
if (_dxc_install_root)
    list(APPEND _dxc_include_hints
        "${_dxc_install_root}/Include/dxc"
        "${_dxc_install_root}/include/dxc"
        "${_dxc_install_root}/Include"
        "${_dxc_install_root}/include"
    )
    list(APPEND _dxc_library_hints
        "${_dxc_install_root}/Lib"
        "${_dxc_install_root}/lib"
        "${_dxc_install_root}/lib/x86_64-linux-gnu"
    )
    if (_dxc_windows_kit_version)
        list(APPEND _dxc_include_hints
            "${_dxc_install_root}/Include/${_dxc_windows_kit_version}/um"
            "${_dxc_install_root}/Include/${_dxc_windows_kit_version}/shared"
        )
        list(APPEND _dxc_library_hints
            "${_dxc_install_root}/Lib/${_dxc_windows_kit_version}/um/x64"
        )
    endif()
endif()

find_path(DXC_INCLUDE_DIR
    NAMES dxcapi.h
    HINTS ${_dxc_include_hints}
    NO_DEFAULT_PATH
    DOC "DXC include directory containing dxcapi.h"
)

find_library(DXC_LIBRARY
    NAMES dxcompiler libdxcompiler
    HINTS ${_dxc_library_hints}
    NO_DEFAULT_PATH
    DOC "DXC import or shared library"
)

if (WIN32 AND _dxc_bin_dir)
    find_file(DXC_DLL
        NAMES dxcompiler.dll
        HINTS "${_dxc_bin_dir}"
        NO_DEFAULT_PATH
        DOC "DXC runtime DLL from the selected compiler installation"
    )
endif()

if (DXC_EXECUTABLE)
    execute_process(
        COMMAND "${DXC_EXECUTABLE}" --version
        OUTPUT_VARIABLE _dxc_version_stdout
        ERROR_VARIABLE _dxc_version_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    set(_dxc_version_output "${_dxc_version_stdout}\n${_dxc_version_stderr}")
    string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" DXC_VERSION "${_dxc_version_output}")
endif()

set(_dxc_required_variables DXC_INCLUDE_DIR DXC_LIBRARY DXC_EXECUTABLE)
if (WIN32)
    list(APPEND _dxc_required_variables DXC_DLL)
endif()
find_package_handle_standard_args(DXC
    REQUIRED_VARS ${_dxc_required_variables}
    VERSION_VAR DXC_VERSION
)

set(DXC_SPIRV_FOUND FALSE)
if (DXC_FOUND)
    set(_dxc_probe_source "${CMAKE_BINARY_DIR}/CMakeFiles/dxc_spirv_probe.hlsl")
    set(_dxc_probe_output "${CMAKE_BINARY_DIR}/CMakeFiles/dxc_spirv_probe.spv")
    file(WRITE "${_dxc_probe_source}"
        "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n"
    )
    file(REMOVE "${_dxc_probe_output}")
    execute_process(
        COMMAND "${DXC_EXECUTABLE}"
            -spirv -T vs_6_0 -E main
            "${_dxc_probe_source}" -Fo "${_dxc_probe_output}"
        RESULT_VARIABLE _dxc_spirv_result
        OUTPUT_VARIABLE _dxc_spirv_stdout
        ERROR_VARIABLE _dxc_spirv_stderr
    )
    if (_dxc_spirv_result EQUAL 0 AND EXISTS "${_dxc_probe_output}")
        file(SIZE "${_dxc_probe_output}" _dxc_probe_size)
        if (_dxc_probe_size GREATER 0)
            set(DXC_SPIRV_FOUND TRUE)
        endif()
    endif()
    if (NOT DXC_SPIRV_FOUND)
        string(STRIP "${_dxc_spirv_stdout}\n${_dxc_spirv_stderr}" DXC_SPIRV_ERROR)
    endif()
endif()
set(DXC_SPIRV_FOUND "${DXC_SPIRV_FOUND}" CACHE BOOL "Selected DXC supports SPIR-V code generation" FORCE)

if (WIN32)
    file(GLOB _dxil_redist_hints LIST_DIRECTORIES TRUE
        "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64"
        "C:/Program Files (x86)/Windows Kits/10/Redist/*/D3D/x64"
    )
    find_file(DXIL_DLL
        NAMES dxil.dll
        HINTS ${_dxil_redist_hints}
        NO_DEFAULT_PATH
        DOC "Windows SDK DXIL validator/signing runtime"
    )
endif()

if (DXC_FOUND)
    if (NOT TARGET DXC::dxcapi)
        add_library(DXC::dxcapi INTERFACE IMPORTED)
        set_target_properties(DXC::dxcapi PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${DXC_INCLUDE_DIR}"
        )
    endif()

    if (NOT TARGET DXC::dxcompiler)
        add_library(DXC::dxcompiler SHARED IMPORTED)
        set_target_properties(DXC::dxcompiler PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${DXC_INCLUDE_DIR}"
        )
        if (WIN32)
            set_target_properties(DXC::dxcompiler PROPERTIES
                IMPORTED_IMPLIB "${DXC_LIBRARY}"
                IMPORTED_LOCATION "${DXC_DLL}"
            )
        else()
            set_target_properties(DXC::dxcompiler PROPERTIES
                IMPORTED_LOCATION "${DXC_LIBRARY}"
            )
        endif()
    endif()
endif()

mark_as_advanced(
    DXC_INCLUDE_DIR DXC_LIBRARY DXC_EXECUTABLE DXC_DLL DXIL_DLL DXC_SPIRV_FOUND
)
