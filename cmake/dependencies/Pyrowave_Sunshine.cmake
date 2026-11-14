set(_pyrowave_source_root "${CMAKE_SOURCE_DIR}/third-party/pyrowave")
set(_pyrowave_granite_source_root "${_pyrowave_source_root}/third_party/Granite")

foreach(_required_file IN ITEMS
        "${_pyrowave_source_root}/CMakeLists.txt"
        "${_pyrowave_granite_source_root}/CMakeLists.txt"
        "${_pyrowave_granite_source_root}/third_party/khronos/vulkan-headers/include/vulkan/vulkan.h"
        "${_pyrowave_granite_source_root}/third_party/volk/volk.h")
    if(NOT EXISTS "${_required_file}")
        message(FATAL_ERROR
                "PyroWave submodules are not initialized; run "
                "git submodule update --init --recursive third-party/pyrowave")
    endif()
endforeach()

function(_sunshine_add_pyrowave)
    # Granite declares cmake_minimum_required(VERSION 3.5), reported as deprecated (< 3.10) by CMake 4. The floor is raised
    # for this third-party subtree only: the variable is local to this function, so it never reaches our own projects
    # (https://cmake.org/cmake/help/latest/variable/CMAKE_POLICY_VERSION_MINIMUM.html).
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(PYROWAVE_DEVEL OFF)
    set(PYROWAVE_UTILS OFF)
    set(PYROWAVE_BUILD_SHARED OFF)
    set(PYROWAVE_BUILD_STATIC_C_API ON)
    set(PYROWAVE_BUILD_TESTS OFF)
    set(PYROWAVE_INSTALL OFF)
    set(GRANITE_INSTALL_TARGETS OFF)
    set(GRANITE_INSTALL_EXE_TARGETS OFF)
    add_subdirectory("${_pyrowave_source_root}" "${CMAKE_BINARY_DIR}/pyrowave" EXCLUDE_FROM_ALL)
endfunction()

_sunshine_add_pyrowave()

add_library(sunshine_pyrowave_runtime INTERFACE)
add_library(Pyrowave::Runtime ALIAS sunshine_pyrowave_runtime)
target_link_libraries(sunshine_pyrowave_runtime INTERFACE pyrowave-c-api-static)
