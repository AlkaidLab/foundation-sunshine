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
