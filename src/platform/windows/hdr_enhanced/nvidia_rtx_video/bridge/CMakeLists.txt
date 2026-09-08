cmake_minimum_required(VERSION 3.20)
project(foundation_rtx_video_bridge LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "The RTX Video bridge requires Windows and MSVC")
endif ()

set(RTX_VIDEO_SDK_ROOT "$ENV{RTX_VIDEO_SDK_ROOT}" CACHE PATH "NVIDIA RTX Video SDK root")
set(_ngx_application_id "$ENV{RTX_VIDEO_NGX_APPLICATION_ID}")
if (_ngx_application_id STREQUAL "")
  set(_ngx_application_id "0")
endif ()
set(RTX_VIDEO_NGX_APPLICATION_ID "${_ngx_application_id}" CACHE STRING "NVIDIA-assigned NGX application ID; defaults to 0")
set(SUNSHINE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." CACHE PATH "Sunshine source tree")
if (NOT RTX_VIDEO_NGX_APPLICATION_ID MATCHES "^(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "RTX_VIDEO_NGX_APPLICATION_ID must be an unsigned decimal integer")
endif ()
string(LENGTH "${RTX_VIDEO_NGX_APPLICATION_ID}" _ngx_id_length)
if (_ngx_id_length GREATER 20 OR
    (_ngx_id_length EQUAL 20 AND RTX_VIDEO_NGX_APPLICATION_ID STRGREATER "18446744073709551615"))
  message(FATAL_ERROR "RTX_VIDEO_NGX_APPLICATION_ID exceeds the NGX unsigned 64-bit range")
endif ()
string(TIMESTAMP RTX_VIDEO_BRIDGE_BUILD_UNIX_SECONDS "%s" UTC)
if (NOT EXISTS "${RTX_VIDEO_SDK_ROOT}/include/nvsdk_ngx_helpers_truehdr.h")
  message(FATAL_ERROR "Set RTX_VIDEO_SDK_ROOT to the extracted NVIDIA RTX Video SDK")
endif ()
if (NOT EXISTS "${SUNSHINE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_rtx_video/bridge_abi.h")
  message(FATAL_ERROR "SUNSHINE_SOURCE_DIR does not contain bridge_abi.h")
endif ()
set(RTX_VIDEO_SDK_IMPORT_LIBRARY
  "${RTX_VIDEO_SDK_ROOT}/lib/Windows/x64/nvsdk_ngx_d.lib")
set(RTX_VIDEO_SDK_TRUEHDR_DLL
  "${RTX_VIDEO_SDK_ROOT}/bin/Windows/x64/rel/nvngx_truehdr.dll")
if (NOT EXISTS "${RTX_VIDEO_SDK_IMPORT_LIBRARY}")
  message(FATAL_ERROR "RTX Video SDK import library not found: ${RTX_VIDEO_SDK_IMPORT_LIBRARY}")
endif ()
if (NOT EXISTS "${RTX_VIDEO_SDK_TRUEHDR_DLL}")
  message(FATAL_ERROR "RTX Video SDK TrueHDR runtime not found: ${RTX_VIDEO_SDK_TRUEHDR_DLL}")
endif ()

add_library(foundation_rtx_video_bridge SHARED src/truehdr_bridge.cpp)
target_compile_features(foundation_rtx_video_bridge PRIVATE cxx_std_20)
target_compile_definitions(foundation_rtx_video_bridge PRIVATE
  FOUNDATION_NGX_APPLICATION_ID=${RTX_VIDEO_NGX_APPLICATION_ID}ULL
  FOUNDATION_RTX_VIDEO_BUILD_UNIX_SECONDS=${RTX_VIDEO_BRIDGE_BUILD_UNIX_SECONDS}LL
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_rtx_video_bridge PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(foundation_rtx_video_bridge SYSTEM PRIVATE
  "${RTX_VIDEO_SDK_ROOT}/include")
target_include_directories(foundation_rtx_video_bridge PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_rtx_video_bridge PRIVATE
  d3d11
  dxgi
  "${RTX_VIDEO_SDK_IMPORT_LIBRARY}")
set_property(TARGET foundation_rtx_video_bridge PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_target_properties(foundation_rtx_video_bridge PROPERTIES
  OUTPUT_NAME foundation_rtx_video_bridge
  PREFIX "")

add_custom_command(TARGET foundation_rtx_video_bridge POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${RTX_VIDEO_SDK_TRUEHDR_DLL}"
    "$<TARGET_FILE_DIR:foundation_rtx_video_bridge>/nvngx_truehdr.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/THIRD_PARTY_NOTICES.md"
    "$<TARGET_FILE_DIR:foundation_rtx_video_bridge>/THIRD_PARTY_NOTICES.md"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/../README.md"
    "$<TARGET_FILE_DIR:foundation_rtx_video_bridge>/README.md")

add_executable(foundation_rtx_video_bridge_smoke tests/bridge_smoke.cpp)
target_compile_features(foundation_rtx_video_bridge_smoke PRIVATE cxx_std_20)
target_compile_definitions(foundation_rtx_video_bridge_smoke PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_rtx_video_bridge_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(foundation_rtx_video_bridge_smoke PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_rtx_video_bridge_smoke PRIVATE d3d11 dxgi)
set_property(TARGET foundation_rtx_video_bridge_smoke PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_dependencies(foundation_rtx_video_bridge_smoke foundation_rtx_video_bridge)
