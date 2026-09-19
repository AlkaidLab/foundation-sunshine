cmake_minimum_required(VERSION 3.20)
project(foundation_dlssnr_adapter LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "The DLSS NR adapter requires Windows and MSVC")
endif ()

set(SUNSHINE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." CACHE PATH "Sunshine source tree")
if (NOT EXISTS "${SUNSHINE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_dlssnr/adapter_abi.h")
  message(FATAL_ERROR "SUNSHINE_SOURCE_DIR does not contain the DLSS NR adapter_abi.h")
endif ()
string(TIMESTAMP DLSSNR_ADAPTER_BUILD_UNIX_SECONDS "%s" UTC)

include("${CMAKE_CURRENT_LIST_DIR}/ngx_sdk.cmake")

# The public SDK provides NGX core initialization and parameter allocation.
# The signed nvngx_dlssnr.dll remains separately supplied at runtime.
add_library(foundation_dlssnr_adapter SHARED src/dlssnr_adapter.cpp)
target_compile_features(foundation_dlssnr_adapter PRIVATE cxx_std_20)
target_compile_definitions(foundation_dlssnr_adapter PRIVATE
  FOUNDATION_DLSSNR_ADAPTER_EXPORTS
  DLSSNR_ADAPTER_BUILD_UNIX_SECONDS=${DLSSNR_ADAPTER_BUILD_UNIX_SECONDS}LL
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_dlssnr_adapter PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(foundation_dlssnr_adapter PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_dlssnr_adapter PRIVATE
  dlssnr_ngx_sdk
  d3d11
  d3d12
  dxgi)
set_property(TARGET foundation_dlssnr_adapter PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_target_properties(foundation_dlssnr_adapter PROPERTIES
  OUTPUT_NAME foundation_dlssnr_adapter
  PREFIX "")

add_executable(foundation_dlssnr_adapter_smoke tests/adapter_smoke.cpp)
target_compile_features(foundation_dlssnr_adapter_smoke PRIVATE cxx_std_20)
target_compile_definitions(foundation_dlssnr_adapter_smoke PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
target_compile_options(foundation_dlssnr_adapter_smoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
target_include_directories(foundation_dlssnr_adapter_smoke PRIVATE
  "${SUNSHINE_SOURCE_DIR}")
target_link_libraries(foundation_dlssnr_adapter_smoke PRIVATE d3d11 dxgi windowscodecs ole32)
set_property(TARGET foundation_dlssnr_adapter_smoke PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
add_dependencies(foundation_dlssnr_adapter_smoke foundation_dlssnr_adapter)

include(CTest)
if (BUILD_TESTING)
  add_executable(foundation_dlssnr_runtime_guard_tests tests/runtime_guard_tests.cpp)
  target_compile_features(foundation_dlssnr_runtime_guard_tests PRIVATE cxx_std_20)
  target_compile_definitions(foundation_dlssnr_runtime_guard_tests PRIVATE
    FOUNDATION_DLSSNR_ADAPTER_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(foundation_dlssnr_runtime_guard_tests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
  target_include_directories(foundation_dlssnr_runtime_guard_tests PRIVATE "${SUNSHINE_SOURCE_DIR}")
  target_link_libraries(foundation_dlssnr_runtime_guard_tests PRIVATE dlssnr_ngx_sdk d3d11 d3d12 dxgi)
  set_property(TARGET foundation_dlssnr_runtime_guard_tests PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  add_test(NAME dlssnr_runtime_guard COMMAND foundation_dlssnr_runtime_guard_tests)
  set_tests_properties(dlssnr_runtime_guard PROPERTIES TIMEOUT 15)
endif ()
