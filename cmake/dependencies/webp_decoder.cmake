include_guard(GLOBAL)
include(FetchContent)

# The system WIC WebP extension may not initialize in the service process.
# Build the pinned decoder statically, without shipping a separate DLL.
foreach(option IN ITEMS WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP
        WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO
        WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(sunshine_webp
    URL https://codeload.github.com/webmproject/libwebp/tar.gz/4fa21912338357f89e4fd51cf2368325b59e9bd9
    URL_HASH SHA256=923f3382a47a2af185c3240c954cf004428b237bd7317413a95146d01eb4b94b)
FetchContent_GetProperties(sunshine_webp)
if(NOT sunshine_webp_POPULATED)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(sunshine_webp)
    cmake_policy(POP)
    block()
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(${sunshine_webp_SOURCE_DIR} ${sunshine_webp_BINARY_DIR} EXCLUDE_FROM_ALL)
    endblock()
endif()
