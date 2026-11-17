include_guard(GLOBAL)
include(FetchContent)

# The WIC WebP extension may fail in a service process. Pin the source and
# build the decoder statically so the fallback and its license are consistent.
FetchContent_Declare(sunshine_webp
    URL https://codeload.github.com/webmproject/libwebp/tar.gz/4fa21912338357f89e4fd51cf2368325b59e9bd9
    URL_HASH SHA256=923f3382a47a2af185c3240c954cf004428b237bd7317413a95146d01eb4b94b
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    # Populate with MakeAvailable, then add the root ourselves so the
    # upstream tools, installation rules and shared-library setting stay local.
    SOURCE_SUBDIR sunshine-no-automatic-build)
FetchContent_MakeAvailable(sunshine_webp)
if(NOT TARGET webpdecoder)
    block()
        set(BUILD_SHARED_LIBS OFF)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        foreach(option IN ITEMS WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP
                WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO
                WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS)
            set(${option} OFF)
        endforeach()
        # Source downloads may be shared; compiled objects belong to this build
        # so separate builds or toolchains do not overwrite one another.
        add_subdirectory("${sunshine_webp_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/sunshine_webp-build" EXCLUDE_FROM_ALL)
    endblock()
endif()
