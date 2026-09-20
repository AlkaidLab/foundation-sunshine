# The DLSS NR adapter is an optional MSVC DLL that hosts the signed
# nvngx_dlssnr snippet on a private D3D12 device. The MinGW host consumes only
# the stable C ABI. The adapter downloads pinned public NGX SDK build inputs;
# the user supplies the separate signed NR runtime.
set(SUNSHINE_DLSS_SDK_ROOT "" CACHE PATH "Local NVIDIA DLSS SDK; empty downloads pinned build inputs")
set(SUNSHINE_DLSSNR "AUTO" CACHE STRING "Build DLSS NR support: AUTO, ON or OFF")
set_property(CACHE SUNSHINE_DLSSNR PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${SUNSHINE_DLSSNR}" _dlssnr_mode)
if (NOT _dlssnr_mode MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "SUNSHINE_DLSSNR must be AUTO, ON or OFF")
endif ()
set(SUNSHINE_DLSSNR_AVAILABLE FALSE CACHE INTERNAL "DLSS NR adapter is configured" FORCE)
if (_dlssnr_mode STREQUAL "OFF" OR NOT WIN32)
    if (_dlssnr_mode STREQUAL "ON" AND NOT WIN32)
        message(FATAL_ERROR "DLSS NR support requires Windows")
    endif ()
    return()
endif ()

set(_dlssnr_source "${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/adapter")
set(_dlssnr_build "${CMAKE_BINARY_DIR}/image_enhancement/nvidia_dlssnr_adapter")
set(DLSSNR_ADAPTER_DLL "${_dlssnr_build}/Release/foundation_dlssnr_adapter.dll")
set(DLSSNR_TRUST_INCLUDE "${CMAKE_BINARY_DIR}/generated/dlssnr")
set(DLSSNR_TRUST_HEADER "${DLSSNR_TRUST_INCLUDE}/dlssnr_trust.h")
set(_dlssnr_adapter_sources
    "${_dlssnr_source}/CMakeLists.txt"
    "${_dlssnr_source}/ngx_sdk.cmake"
    "${_dlssnr_source}/src/dlssnr_adapter.cpp"
    "${_dlssnr_source}/src/nvof_provider.cpp"
    "${_dlssnr_source}/src/nvof_provider.h"
    "${_dlssnr_source}/include/nvof/nvOpticalFlowCommon.h"
    "${_dlssnr_source}/include/nvof/nvOpticalFlowD3D11.h"
    "${_dlssnr_source}/NVIDIA-OPTICAL-FLOW-NOTICES.txt"
    "${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/adapter_abi.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_dlssnr_adapter_sources})

file(SHA256 "${_dlssnr_source}/CMakeLists.txt" _dlssnr_cmake_hash)
file(SHA256 "${_dlssnr_source}/src/dlssnr_adapter.cpp" _dlssnr_source_hash)
file(SHA256 "${_dlssnr_source}/ngx_sdk.cmake" _dlssnr_sdk_hash)
file(SHA256 "${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/adapter_abi.h" _dlssnr_abi_hash)
string(SHA256 _dlssnr_inputs
    "${_dlssnr_cmake_hash}|${_dlssnr_source_hash}|${_dlssnr_abi_hash}|${_dlssnr_sdk_hash}|${SUNSHINE_DLSS_SDK_ROOT}")
foreach (_dlssnr_input IN LISTS _dlssnr_adapter_sources)
    file(SHA256 "${_dlssnr_input}" _dlssnr_input_hash)
    string(SHA256 _dlssnr_inputs "${_dlssnr_inputs}|${_dlssnr_input_hash}")
endforeach ()
set(_dlssnr_previous "")
if (EXISTS "${_dlssnr_build}/configure-inputs")
    file(READ "${_dlssnr_build}/configure-inputs" _dlssnr_previous)
endif ()
if (NOT _dlssnr_inputs STREQUAL _dlssnr_previous OR NOT EXISTS "${_dlssnr_build}/CMakeCache.txt")
    set(_dlssnr_configured "1")
    set(_dlssnr_configure_log "")
    foreach (_dlssnr_generator IN ITEMS "Visual Studio 18 2026" "Visual Studio 17 2022")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dlssnr_source}" -B "${_dlssnr_build}"
            -G "${_dlssnr_generator}" -A x64
            "-DSUNSHINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DSUNSHINE_DLSS_SDK_ROOT=${SUNSHINE_DLSS_SDK_ROOT}"
            RESULT_VARIABLE _dlssnr_configured OUTPUT_VARIABLE _dlssnr_stdout ERROR_VARIABLE _dlssnr_stderr TIMEOUT 120)
        string(APPEND _dlssnr_configure_log
            "generator=${_dlssnr_generator}\nresult=${_dlssnr_configured}\nstdout:\n${_dlssnr_stdout}\nstderr:\n${_dlssnr_stderr}\n")
        if (_dlssnr_configured STREQUAL "0")
            break()
        endif ()

        # A failed configure may leave a generator-specific cache behind. Remove
        # only the generated metadata before trying the older supported toolset.
        file(REMOVE_RECURSE "${_dlssnr_build}/CMakeCache.txt" "${_dlssnr_build}/CMakeFiles")
    endforeach ()
    if (NOT _dlssnr_configured STREQUAL "0")
        file(MAKE_DIRECTORY "${_dlssnr_build}")
        file(WRITE "${_dlssnr_build}/configure.log" "${_dlssnr_configure_log}")
        if (_dlssnr_mode STREQUAL "ON")
            message(FATAL_ERROR
                "DLSS NR adapter configuration failed:\n${_dlssnr_configure_log}")
        endif ()
        message(STATUS "DLSS NR disabled: adapter configuration failed; see ${_dlssnr_build}/configure.log")
        return()
    endif ()
    file(WRITE "${_dlssnr_build}/configure-inputs" "${_dlssnr_inputs}")
endif ()

file(MAKE_DIRECTORY "${DLSSNR_TRUST_INCLUDE}")
add_custom_command(
    OUTPUT "${DLSSNR_TRUST_HEADER}"
    COMMAND "${CMAKE_COMMAND}" --build "${_dlssnr_build}" --config Release
        --target foundation_dlssnr_adapter
    COMMAND "${CMAKE_COMMAND}"
        "-DADAPTER_PATH=${DLSSNR_ADAPTER_DLL}"
        "-DOUTPUT_PATH=${DLSSNR_TRUST_HEADER}"
        -P "${CMAKE_CURRENT_LIST_DIR}/GenerateDlssnrTrustHeader.cmake"
    DEPENDS ${_dlssnr_adapter_sources}
        "${_dlssnr_build}/configure-inputs"
        "${CMAKE_CURRENT_LIST_DIR}/GenerateDlssnrTrustHeader.cmake"
    BYPRODUCTS "${DLSSNR_ADAPTER_DLL}"
    COMMENT "Building and fingerprinting the optional MSVC DLSS NR adapter"
    VERBATIM)
add_custom_target(sunshine_dlssnr_adapter DEPENDS "${DLSSNR_TRUST_HEADER}")
set(SUNSHINE_DLSSNR_AVAILABLE TRUE CACHE INTERNAL "DLSS NR adapter is configured" FORCE)
message(STATUS "DLSS NR support enabled; the adapter is fingerprinted at build time and the NVIDIA runtime remains optional at run time")
