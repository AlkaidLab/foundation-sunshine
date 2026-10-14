# common compile definitions
# this file will also load platform specific definitions

list(APPEND SUNSHINE_COMPILE_OPTIONS -Wall -Wno-sign-compare)
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options

    # GCC 12 and higher will complain about maybe-uninitialized
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-maybe-uninitialized)

        # Disable the bogus warning that may prevent compilation (only for GCC 12).
        # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-restrict)
        endif()
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
endif()
if(BUILD_WERROR)
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Werror)
endif()

# setup assets directory
if(NOT SUNSHINE_ASSETS_DIR)
    set(SUNSHINE_ASSETS_DIR "assets")
endif()

# platform specific compile definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/compile_definitions/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/compile_definitions/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/compile_definitions/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/compile_definitions/linux.cmake)
    endif()
endif()

configure_file("${CMAKE_SOURCE_DIR}/src/version.h.in" version.h @ONLY)

if(NOT SUNSHINE_ASSETS_DIR_DEF)
    set(SUNSHINE_ASSETS_DIR_DEF "${SUNSHINE_ASSETS_DIR}")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR_DEF}")

list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY=${SUNSHINE_TRAY})
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_GUI_TRAY=${SUNSHINE_GUI_TRAY})

# Publisher metadata - escape spaces for proper compilation
string(REPLACE " " "_" SUNSHINE_PUBLISHER_NAME_SAFE "${SUNSHINE_PUBLISHER_NAME}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_NAME="${SUNSHINE_PUBLISHER_NAME_SAFE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_WEBSITE="${SUNSHINE_PUBLISHER_WEBSITE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_ISSUE_URL="${SUNSHINE_PUBLISHER_ISSUE_URL}")

# Collect application-only platform includes without leaking them to HTTP or
# other reusable modules. Keep project headers outside SYSTEM warning suppression.
get_directory_property(_application_includes INCLUDE_DIRECTORIES)
list(REMOVE_ITEM _application_includes "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
add_library(sunshine_build_dependencies INTERFACE)
add_library(sunshine_ffmpeg_headers INTERFACE)
add_library(sunshine::ffmpeg_headers ALIAS sunshine_ffmpeg_headers)
target_include_directories(sunshine_ffmpeg_headers SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
add_library(sunshine_host_headers INTERFACE)
add_library(sunshine::host_headers ALIAS sunshine_host_headers)
target_include_directories(sunshine_host_headers SYSTEM INTERFACE
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include")
target_link_libraries(sunshine_host_headers INTERFACE sunshine::ffmpeg_headers)
add_library(sunshine_ffmpeg INTERFACE)
add_library(sunshine::ffmpeg ALIAS sunshine_ffmpeg)
target_link_libraries(sunshine_ffmpeg INTERFACE sunshine::ffmpeg_headers ${FFMPEG_LIBRARIES})
if(WIN32)
    # libavutil uses BCrypt for random bytes; keep it after the static archive.
    target_link_libraries(sunshine_ffmpeg INTERFACE bcrypt)
endif()
target_include_directories(sunshine_build_dependencies INTERFACE
        "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(sunshine_build_dependencies SYSTEM INTERFACE
        ${_application_includes}
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        "$<$<PLATFORM_ID:Darwin>:${FFMPEG_INCLUDE_DIRS}>"
        ${MINIUPNP_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})
target_link_libraries(sunshine_build_dependencies INTERFACE
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        ${MINIUPNP_LIBRARIES} Threads::Threads enet moonlight::haptics
        nlohmann_json::nlohmann_json opus sunshine::ffmpeg ${Boost_LIBRARIES}
        sunshine::tls sunshine::curl ${PLATFORM_LIBRARIES})
if(WIN32)
    target_compile_definitions(sunshine_build_dependencies INTERFACE MINIUPNP_STATICLIB)
endif()
set(SUNSHINE_EXTERNAL_LIBRARIES sunshine_build_dependencies)
