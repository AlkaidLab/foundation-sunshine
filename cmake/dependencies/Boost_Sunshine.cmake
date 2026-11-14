#
# Loads the boost library giving the priority to the system package first, with a fallback to FetchContent.
#
include_guard(GLOBAL)

if(NOT COMMAND sunshine_source_group_filter_installed)
    macro(sunshine_source_group_filter_installed)
    endmacro()
    function(source_group)
        get_property(_sunshine_filter GLOBAL PROPERTY SUNSHINE_FILTER_SOURCE_GROUP_TREE)
        if(NOT _sunshine_filter OR NOT ARGV0 STREQUAL "TREE")
            _source_group(${ARGN})
            return()
        endif()
        cmake_parse_arguments(_sunshine_sg "" "TREE;PREFIX" "FILES" ${ARGN})
        set(_sunshine_sg_kept "")
        foreach(_sunshine_sg_file IN LISTS _sunshine_sg_FILES)
            cmake_path(IS_PREFIX _sunshine_sg_TREE "${_sunshine_sg_file}" NORMALIZE _sunshine_sg_inside)
            if(_sunshine_sg_inside)
                list(APPEND _sunshine_sg_kept "${_sunshine_sg_file}")
            endif()
        endforeach()
        if(_sunshine_sg_kept)
            if(DEFINED _sunshine_sg_PREFIX)
                _source_group(TREE "${_sunshine_sg_TREE}" PREFIX "${_sunshine_sg_PREFIX}" FILES ${_sunshine_sg_kept})
            else()
                _source_group(TREE "${_sunshine_sg_TREE}" FILES ${_sunshine_sg_kept})
            endif()
        endif()
    endfunction()
endif()


set(BOOST_VERSION "1.92.0")
set(BOOST_RELEASE_VERSION "1.92.0")
set(BOOST_COMPONENTS
        atomic
        crc
        format
        property_tree
        asio
        beast
        filesystem
        function
        locale
        math
        log
        process
        program_options
        system
        thread
)
# Networking uses Boost.System error codes and their compatibility target.

# Additional components used by the Windows sources.
if(WIN32)
    list(APPEND BOOST_COMPONENTS
            algorithm
            preprocessor
            uuid
    )
endif()

if(BOOST_USE_STATIC)
    set(Boost_USE_STATIC_LIBS ON)  # cmake-lint: disable=C0103
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    cmake_policy(SET CMP0167 NEW)  # Get BoostConfig.cmake from upstream
endif()
find_package(Boost CONFIG ${BOOST_VERSION} EXACT COMPONENTS ${BOOST_COMPONENTS})
if(Boost_FOUND AND NOT TARGET Boost::headers AND NOT TARGET Boost::boost)
    # A stale FetchContent redirect can report a package without restoring its
    # targets on a subsequent configure. Re-enter the pinned source path.
    set(Boost_FOUND FALSE)
endif()
if(NOT Boost_FOUND)
    message(STATUS "Boost v${BOOST_VERSION} package not found in the system. Falling back to FetchContent.")
    include(FetchContent)

    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        cmake_policy(SET CMP0135 NEW)  # Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24
    endif()
    if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.0")
        cmake_policy(SET CMP0174 NEW)  # Handle empty variables
    endif()

    set(BOOST_ENABLE_CMAKE ON)

    if(WIN32)
        # The Windows backend already provides the locale services Sunshine uses.
        # Enabling a host MSYS2 ICU installation makes the portable executable
        # depend on versioned development-environment DLLs that are not shipped.
        set(BOOST_LOCALE_ENABLE_ICU OFF CACHE BOOL "Build Boost.Locale without ICU on Windows" FORCE)
    endif()

    # Limit boost to the required libraries only
    set(BOOST_INCLUDE_LIBRARIES ${BOOST_COMPONENTS})
    set(BOOST_URL "https://github.com/boostorg/boost/releases/download/boost-${BOOST_RELEASE_VERSION}/boost-${BOOST_RELEASE_VERSION}-cmake.tar.xz")  # cmake-lint: disable=C0301
    set(BOOST_HASH "SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d")

    if(CMAKE_VERSION VERSION_LESS "3.24.0")
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
        )
    elseif(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.25.0")
        # add SYSTEM to FetchContent_Declare, this fails on debian bookworm
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
                SYSTEM  # requires CMake 3.25+
                OVERRIDE_FIND_PACKAGE  # requires CMake 3.24+, but we have a macro to handle it for other versions
        )
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        FetchContent_Declare(
                Boost
                URL ${BOOST_URL}
                URL_HASH ${BOOST_HASH}
                OVERRIDE_FIND_PACKAGE  # requires CMake 3.24+, but we have a macro to handle it for other versions
        )
    endif()

    # CMake 4.5 rejects a source_group(TREE) entry whose file lies outside the
    # tree; Boost.Locale 1.92 lists include/boost/locale.hpp under the tree
    # include/boost/locale. source_group only shapes IDE folders, so while Boost
    # is configured, files outside their TREE are left out of the grouping.
    set_property(GLOBAL PROPERTY SUNSHINE_FILTER_SOURCE_GROUP_TREE ON)
    FetchContent_MakeAvailable(Boost)
    set_property(GLOBAL PROPERTY SUNSHINE_FILTER_SOURCE_GROUP_TREE OFF)
    set(FETCH_CONTENT_BOOST_USED TRUE)

    add_definitions(-DBOOST_SYSTEM_USE_UTF8)
    message(STATUS "Boost.System UTF-8 support enabled: BOOST_SYSTEM_USE_UTF8")

    set(Boost_FOUND TRUE)  # cmake-lint: disable=C0103
    set(Boost_INCLUDE_DIRS  # cmake-lint: disable=C0103
            "$<BUILD_INTERFACE:${Boost_SOURCE_DIR}/libs/headers/include>")

    if(WIN32)
        # Windows build is failing to create .h file in this directory
        file(MAKE_DIRECTORY ${Boost_BINARY_DIR}/libs/log/src/windows)
    endif()

    set(Boost_LIBRARIES "")  # cmake-lint: disable=C0103
    foreach(component ${BOOST_COMPONENTS})
        list(APPEND Boost_LIBRARIES "Boost::${component}")
    endforeach()
endif()

# Keep the historical Boost::boost spelling available to dependencies
# while sharing the modern Boost::headers implementation.
if(TARGET Boost::headers AND NOT TARGET Boost::boost)
    add_library(sunshine_boost_headers INTERFACE)
    target_link_libraries(sunshine_boost_headers INTERFACE Boost::headers)
    add_library(Boost::boost ALIAS sunshine_boost_headers)
endif()

message(STATUS "Boost include dirs: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
