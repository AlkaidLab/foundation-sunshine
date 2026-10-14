# load common dependencies
# this file will also load platform specific dependencies

# Shared networking and application dependencies
include(dependencies/Boost_Sunshine)

# submodules
# moonlight common library
set(ENET_NO_INSTALL ON CACHE BOOL "Don't install any libraries built for enet")
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet")

# Authored haptics analysis belongs to the unprivileged Sunshine process. The
# elevated DualSense sidecar and driver remain independent of this SDK.
set(MOONLIGHT_HAPTICS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/moonlight-audio-haptics" EXCLUDE_FROM_ALL)


# common dependencies
include("${CMAKE_MODULE_PATH}/dependencies/nlohmann_json.cmake")
if(WIN32)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    # An older configure may have cached DLL import libraries. Preserve explicit
    # static paths; re-discover only incompatible cached dynamic selections.
    foreach(_library IN ITEMS OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
            SSL_EAY LIB_EAY SSL_EAY_RELEASE SSL_EAY_DEBUG LIB_EAY_RELEASE LIB_EAY_DEBUG)
        if(DEFINED ${_library} AND "${${_library}}" MATCHES "\\.dll\\.a$")
            unset(${_library} CACHE)
            unset(${_library})
        endif()
    endforeach()
endif()
find_package(OpenSSL REQUIRED)
if(MINGW AND (OPENSSL_SSL_LIBRARY MATCHES "\\.dll\\.a$" OR OPENSSL_CRYPTO_LIBRARY MATCHES "\\.dll\\.a$"))
    message(FATAL_ERROR "Sunshine requires static OpenSSL libraries on MinGW")
endif()
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
include("${CMAKE_MODULE_PATH}/dependencies/network.cmake")

# miniupnp
pkg_check_modules(MINIUPNP miniupnpc REQUIRED)


# ffmpeg pre-compiled binaries
if(WIN32)
    set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl MinHook)
elseif(UNIX AND NOT APPLE)
    set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
endif()

if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    set(FFMPEG_PREPARED_BINARIES
            "${CMAKE_SOURCE_DIR}/third-party/build-deps/dist/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

    # check if the directory exists
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}")
        message(FATAL_ERROR
                "FFmpeg pre-compiled binaries not found at ${FFMPEG_PREPARED_BINARIES}. \
                Please consider contributing to the LizardByte/build-deps repository. \
                Optionally, you can use the FFMPEG_PREPARED_BINARIES option to specify the path to the \
                system-installed FFmpeg libraries")
    endif()

    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY
                "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()
    set(FFMPEG_LIBRARIES
            "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
            "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
            ${HDR10_PLUS_LIBRARY}
            ${FFMPEG_PLATFORM_LIBRARIES})
else()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY
                "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
        ${HDR10_PLUS_LIBRARY}
        ${FFMPEG_PLATFORM_LIBRARIES})
endif()

set(FFMPEG_INCLUDE_DIRS
        "${FFMPEG_PREPARED_BINARIES}/include")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    if(APPLE)
        include("${CMAKE_MODULE_PATH}/dependencies/macos.cmake")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
    endif()
endif()
