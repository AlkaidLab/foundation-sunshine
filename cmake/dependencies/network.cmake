include_guard(GLOBAL)

# Imported OpenSSL targets own both their headers and library paths. Windows
# discovery is explicitly static before find_package(), including cached builds.
add_library(sunshine_tls INTERFACE)
add_library(sunshine::tls ALIAS sunshine_tls)
target_link_libraries(sunshine_tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)

add_library(sunshine_curl INTERFACE)
add_library(sunshine::curl ALIAS sunshine_curl)
if(WIN32)
    target_include_directories(sunshine_curl SYSTEM INTERFACE ${CURL_STATIC_INCLUDE_DIRS})
    target_compile_definitions(sunshine_curl INTERFACE CURL_STATICLIB)
    target_link_directories(sunshine_curl INTERFACE ${CURL_STATIC_LIBRARY_DIRS})
    target_link_libraries(sunshine_curl INTERFACE ${CURL_STATIC_LIBRARIES})
    target_link_options(sunshine_curl INTERFACE ${CURL_STATIC_LDFLAGS_OTHER})
else()
    target_link_libraries(sunshine_curl INTERFACE PkgConfig::CURL)
endif()

# gzip for the session log that goes with a crash or feedback report (src/crash/crash_log.cpp). On Windows the static libcurl link line already carries -lz
# (MSYS2 mingw-w64-ucrt-x86_64-zlib, libz.a with -static; the runtime zlib1.dll is installed for OpenSSL, cmake/packaging/windows.cmake): a second zlib
# from find_package would be the import library next to it and the link would define every symbol twice. Elsewhere curl is linked dynamically and brings no zlib.
if(NOT WIN32)
    find_package(ZLIB REQUIRED)
    target_link_libraries(sunshine_curl INTERFACE ZLIB::ZLIB)
endif()
