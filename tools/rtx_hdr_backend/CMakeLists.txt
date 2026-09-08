cmake_minimum_required(VERSION 3.20)
project(foundation_truehdr_backend LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "The RTX Video SDK backend requires Windows and MSVC")
endif ()

set(RTX_VIDEO_SDK_ROOT "$ENV{RTX_VIDEO_SDK_ROOT}" CACHE PATH "NVIDIA RTX Video SDK root")
set(SUNSHINE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Sunshine source tree")
if (NOT EXISTS "${RTX_VIDEO_SDK_ROOT}/include/nvsdk_ngx_helpers_truehdr.h")
  message(FATAL_ERROR "Set RTX_VIDEO_SDK_ROOT to the extracted NVIDIA RTX Video SDK")
endif ()
if (NOT EXISTS "${SUNSHINE_SOURCE_DIR}/src/platform/windows/rtx_hdr/backend_abi.h")
  message(FATAL_ERROR "SUNSHINE_SOURCE_DIR does not contain backend_abi.h")
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

add_library(foundation_truehdr_backend SHARED src/backend.cpp)
target_compile_features(foundation_truehdr_backend PRIVATE cxx_std_20)
target_compile_definitions(foundation_truehdr_backend PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_truehdr_backend PRIVATE /W4 /WX /permissive- /EHsc)
target_include_directories(foundation_truehdr_backend SYSTEM PRIVATE
  "${RTX_VIDEO_SDK_ROOT}/include")
target_include_directories(foundation_truehdr_backend PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_truehdr_backend PRIVATE
  d3d11
  dxgi
  "${RTX_VIDEO_SDK_IMPORT_LIBRARY}")
set_property(TARGET foundation_truehdr_backend PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_target_properties(foundation_truehdr_backend PROPERTIES
  OUTPUT_NAME foundation_truehdr_backend
  PREFIX "")

add_custom_command(TARGET foundation_truehdr_backend POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${RTX_VIDEO_SDK_TRUEHDR_DLL}"
    "$<TARGET_FILE_DIR:foundation_truehdr_backend>/nvngx_truehdr.dll")

add_executable(foundation_truehdr_backend_smoke tests/backend_smoke.cpp)
target_compile_features(foundation_truehdr_backend_smoke PRIVATE cxx_std_20)
target_compile_definitions(foundation_truehdr_backend_smoke PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_truehdr_backend_smoke PRIVATE /W4 /WX /permissive- /EHsc)
target_include_directories(foundation_truehdr_backend_smoke PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_truehdr_backend_smoke PRIVATE d3d11 dxgi)
set_property(TARGET foundation_truehdr_backend_smoke PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_dependencies(foundation_truehdr_backend_smoke foundation_truehdr_backend)
