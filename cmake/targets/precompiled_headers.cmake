# fs-cmake-target-pch-20261116: precompiled headers for targets with enough C++ sources to amortise the
# one-time parse. The Windows CI release job enables this for GCC and Clang.
if(NOT SUNSHINE_PRECOMPILED_HEADERS)
    return()
endif()

set(SUNSHINE_PCH_MINIMUM_SOURCES 8 CACHE STRING "Smallest number of C++ sources a target needs for the common PCH")

function(_sunshine_collect_targets directory result)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _sunshine_collect_targets("${_subdirectory}" _inner)
        list(APPEND _targets ${_inner})
    endforeach()
    set(${result} "${_targets}" PARENT_SCOPE)
endfunction()

function(_sunshine_apply_precompiled_headers)
    _sunshine_collect_targets("${CMAKE_SOURCE_DIR}" _all_targets)
    foreach(_target IN LISTS _all_targets)
        get_target_property(_type ${_target} TYPE)
        get_target_property(_source_dir ${_target} SOURCE_DIR)
        if(NOT _type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$" OR _source_dir MATCHES "_deps|third-party|googletest")
            continue()
        endif()
        get_target_property(_sources ${_target} SOURCES)
        set(_cxx_sources ${_sources})
        list(FILTER _cxx_sources INCLUDE REGEX "\\.cpp$")
        list(LENGTH _cxx_sources _count)
        if(_count LESS SUNSHINE_PCH_MINIMUM_SOURCES)
            continue()
        endif()
        message(STATUS "Precompiled header for ${_target} (${_count} C++ sources)")
        target_precompile_headers(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/src/precompiled.h>")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-Xclang -fno-pch-timestamp>")
        endif()
    endforeach()
endfunction()

# fs-cmake-target-pch-20261116: this source must establish its own Windows
# HID header order; a PCH would hide the macro-controlled windows.h contents
# before hidpi.h defines USAGE and NTSTATUS.
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_mouse.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _sunshine_apply_precompiled_headers)
