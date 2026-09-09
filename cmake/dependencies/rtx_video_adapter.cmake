# SDK C++ stays inside the MSVC archive; the host only consumes its C ABI.
set(SUNSHINE_RTX_HDR "AUTO" CACHE STRING "Build RTX HDR support: AUTO, ON or OFF")
set_property(CACHE SUNSHINE_RTX_HDR PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${SUNSHINE_RTX_HDR}" _rtx_mode)
if (NOT _rtx_mode MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "SUNSHINE_RTX_HDR must be AUTO, ON or OFF")
endif ()
set(SUNSHINE_RTX_HDR_AVAILABLE FALSE CACHE INTERNAL "RTX HDR adapter is configured" FORCE)
if (_rtx_mode STREQUAL "OFF" OR NOT WIN32)
    unset(_SUNSHINE_RTX_VIDEO_SDK_TOKEN)
    if (_rtx_mode STREQUAL "ON" AND NOT WIN32)
        message(FATAL_ERROR "RTX HDR support requires Windows")
    endif ()
    return()
endif ()

include("${CMAKE_CURRENT_LIST_DIR}/FetchRtxVideoSdk.cmake")
sunshine_find_rtx_video_sdk(_rtx_sdk_root _rtx_reason)
unset(_SUNSHINE_RTX_VIDEO_SDK_TOKEN)
find_program(RTX_VIDEO_LLD NAMES ld.lld)
if (NOT _rtx_sdk_root OR NOT RTX_VIDEO_LLD)
    if (_rtx_sdk_root AND NOT RTX_VIDEO_LLD)
        set(_rtx_reason "LLD is unavailable; install mingw-w64-ucrt-x86_64-lld")
    endif ()
    if (_rtx_mode STREQUAL "ON")
        message(FATAL_ERROR "RTX HDR is required, but ${_rtx_reason}")
    endif ()
    message(STATUS "RTX HDR disabled: ${_rtx_reason}")
    return()
endif ()

set(RTX_VIDEO_NGX_APPLICATION_ID "" CACHE STRING "NGX application ID; empty uses the environment or development ID 0")
set(_rtx_app_id "$ENV{RTX_VIDEO_NGX_APPLICATION_ID}")
if (_rtx_app_id STREQUAL "")
    set(_rtx_app_id "${RTX_VIDEO_NGX_APPLICATION_ID}")
endif ()
if (_rtx_app_id STREQUAL "")
    set(_rtx_app_id "0")
endif ()
# The application ID is compiled into the binary, not an access credential.
# Retain it for Ninja-triggered reconfiguration after the CI environment is gone.
set(RTX_VIDEO_NGX_APPLICATION_ID "${_rtx_app_id}" CACHE STRING "NGX application ID" FORCE)
set(_rtx_source "${CMAKE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rtx_source}/CMakeLists.txt"
    "${_rtx_sdk_root}/bin/Windows/x64/rel/nvngx_truehdr.dll"
    "${_rtx_sdk_root}/lib/Windows/x64/nvsdk_ngx_d.lib")
set(_rtx_build "${CMAKE_BINARY_DIR}/hdr_enhanced/nvidia_rtx_video_adapter")
unset(RTX_VIDEO_STATIC_CONFIG CACHE)
set(RTX_VIDEO_STATIC_CONFIG "${_rtx_build}/Release/static-adapter.cmake")
file(SHA256 "${_rtx_source}/CMakeLists.txt" _rtx_cmake_hash)
file(SHA256 "${_rtx_sdk_root}/bin/Windows/x64/rel/nvngx_truehdr.dll" _rtx_runtime_hash)
file(SHA256 "${_rtx_sdk_root}/lib/Windows/x64/nvsdk_ngx_d.lib" _rtx_client_hash)
string(SHA256 _rtx_inputs "${_rtx_sdk_root}|${_rtx_app_id}|${_rtx_cmake_hash}|${_rtx_runtime_hash}|${_rtx_client_hash}")
set(_rtx_previous "")
if (EXISTS "${_rtx_build}/configure-inputs")
    file(READ "${_rtx_build}/configure-inputs" _rtx_previous)
endif ()
if (NOT _rtx_inputs STREQUAL _rtx_previous OR NOT EXISTS "${RTX_VIDEO_STATIC_CONFIG}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_rtx_source}" -B "${_rtx_build}"
        -G "Visual Studio 17 2022" -A x64
        "-DNVIDIA_RTX_VIDEO_SDK_DIR=${_rtx_sdk_root}"
        "-DRTX_VIDEO_NGX_APPLICATION_ID=${_rtx_app_id}"
        "-DSUNSHINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rtx_configured OUTPUT_VARIABLE _rtx_stdout ERROR_VARIABLE _rtx_stderr TIMEOUT 120)
    if (NOT _rtx_configured STREQUAL "0" OR NOT EXISTS "${RTX_VIDEO_STATIC_CONFIG}")
        file(MAKE_DIRECTORY "${_rtx_build}")
        file(WRITE "${_rtx_build}/configure.log"
            "result=${_rtx_configured}\nstdout:\n${_rtx_stdout}\nstderr:\n${_rtx_stderr}")
        if (_rtx_mode STREQUAL "ON")
            message(FATAL_ERROR "RTX HDR adapter configuration failed; see ${_rtx_build}/configure.log")
        endif ()
        message(STATUS "RTX HDR disabled: adapter configuration failed; see ${_rtx_build}/configure.log")
        return()
    endif ()
    file(WRITE "${_rtx_build}/configure-inputs" "${_rtx_inputs}")
endif ()

if (EXISTS "${RTX_VIDEO_STATIC_CONFIG}")
    include("${RTX_VIDEO_STATIC_CONFIG}")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libmsvcrt.a
        OUTPUT_VARIABLE _mingw_msvcrt OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if (NOT EXISTS "${_mingw_msvcrt}")
        message(FATAL_ERROR "RTX Video adapter requires the UCRT64 host runtime archive")
    endif ()
    get_filename_component(_lld_dir "${RTX_VIDEO_LLD}" DIRECTORY)
    get_filename_component(_adapter_dir "${RTX_VIDEO_STATIC_LIBRARY}" DIRECTORY)
    configure_file("${_mingw_msvcrt}" "${RTX_VIDEO_LINK_SUPPORT}/libmingw_ucrt.a" COPYONLY)
    add_library(sunshine_rtx_video_adapter INTERFACE)
    add_custom_target(rtx_video_adapter_build
        COMMAND "${CMAKE_COMMAND}" --build "${RTX_VIDEO_ADAPTER_BUILD_DIR}" --config Release
            --target foundation_rtx_video_adapter
        BYPRODUCTS "${RTX_VIDEO_STATIC_LIBRARY}"
        COMMENT "Building the MSVC NGX static adapter"
        VERBATIM)
    add_dependencies(sunshine_rtx_video_adapter rtx_video_adapter_build)
    target_include_directories(sunshine_rtx_video_adapter INTERFACE "${RTX_VIDEO_GENERATED_INCLUDE}")
    target_compile_definitions(sunshine_rtx_video_adapter INTERFACE SUNSHINE_RTX_VIDEO_STATIC)
    target_link_options(sunshine_rtx_video_adapter INTERFACE
        "-B${_lld_dir}/" -fuse-ld=lld "-Wl,-u,__local_stdio_printf_options")
    target_link_directories(sunshine_rtx_video_adapter INTERFACE "${RTX_VIDEO_LINK_SUPPORT}" "${_adapter_dir}")
    set_property(TARGET sunshine_rtx_video_adapter PROPERTY INTERFACE_LINK_DEPENDS
        "${RTX_VIDEO_STATIC_LIBRARY};${RTX_VIDEO_LINK_SUPPORT}/libngx_client.a")
    # Host CRT owns startup/atexit; MSVC supplies only the SDK's remaining runtime symbols.
    # Named archives also avoid GCC forwarding space-escaped .lib paths incorrectly to LLD.
    target_link_libraries(sunshine_rtx_video_adapter INTERFACE
        mingw32 -l:foundation_rtx_video_adapter.lib -l:libngx_client.a -l:libmingw_ucrt.a
        -l:libmsvcprt.a -l:libvcruntime.a
        -l:libmsvcrt.a -l:liblegacy_stdio_definitions.a
        d3d11 dxgi shlwapi version bcrypt)
    # Manual hardware verification: never run this target automatically in headless CTest.
    add_executable(rtx_video_static_smoke EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter/tests/adapter_smoke.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_rtx_video/runtime_loader.cpp")
    target_include_directories(rtx_video_static_smoke PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_features(rtx_video_static_smoke PRIVATE cxx_std_23)
    target_compile_definitions(rtx_video_static_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(rtx_video_static_smoke PRIVATE sunshine_rtx_video_adapter Boost::scope)
    target_link_options(rtx_video_static_smoke PRIVATE -municode)
    add_custom_command(TARGET rtx_video_static_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${RTX_VIDEO_REDIST_FILES} "$<TARGET_FILE_DIR:rtx_video_static_smoke>"
        COMMAND_EXPAND_LISTS VERBATIM)
    set(SUNSHINE_RTX_HDR_AVAILABLE TRUE CACHE INTERNAL "RTX HDR adapter is configured" FORCE)
    message(STATUS "RTX HDR support enabled; NVIDIA runtime remains optional at run time")
endif ()
