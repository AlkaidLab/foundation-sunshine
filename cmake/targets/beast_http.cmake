# Shared application HTTP transport. Beast owns parsing and serialization;
# route modules supply business handlers and explicit TLS identity policy.
add_library(sunshine_http STATIC
        ${CMAKE_SOURCE_DIR}/src/http/types.cpp
        ${CMAKE_SOURCE_DIR}/src/http/server.cpp
        ${CMAKE_SOURCE_DIR}/src/http/client.cpp
        ${CMAKE_SOURCE_DIR}/src/http/tls_trust.cpp)
target_include_directories(sunshine_http PUBLIC "${CMAKE_SOURCE_DIR}" ${OPENSSL_INCLUDE_DIR})
# Windows selects static OpenSSL in compile_definitions/windows.cmake. The
# imported FindOpenSSL targets can still point to DLL import libraries; mixing
# those with the host's static crypto library corrupts opaque OpenSSL objects.
target_link_libraries(sunshine_http PUBLIC Boost::asio Boost::beast ${OPENSSL_LIBRARIES})
target_include_directories(sunshine_http PRIVATE ${CURL_INCLUDE_DIRS})
if (WIN32)
    target_link_libraries(sunshine_http PRIVATE ${CURL_STATIC_LIBRARIES})
else ()
    target_link_libraries(sunshine_http PRIVATE ${CURL_LIBRARIES})
endif ()
set_target_properties(sunshine_http PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_compile_definitions(sunshine_http PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(sunshine_http PUBLIC ws2_32 PRIVATE crypt32)
endif ()
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES sunshine_http)
