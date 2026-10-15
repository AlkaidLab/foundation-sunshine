# Shared application HTTP transport. Beast owns parsing and serialization;
# route modules supply business handlers and explicit TLS identity policy.
add_library(sunshine_http STATIC
        ${CMAKE_SOURCE_DIR}/src/http/types.cpp
        ${CMAKE_SOURCE_DIR}/src/http/server.cpp
        ${CMAKE_SOURCE_DIR}/src/http/client.cpp
        ${CMAKE_SOURCE_DIR}/src/http/tls_trust.cpp)
target_include_directories(sunshine_http PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(sunshine_http PUBLIC Boost::asio Boost::beast sunshine::tls
        PRIVATE sunshine::curl sunshine_project_options sunshine_coverage)
target_compile_features(sunshine_http PUBLIC cxx_std_23)
if (WIN32)
    target_compile_definitions(sunshine_http PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(sunshine_http PUBLIC ws2_32 PRIVATE crypt32)
endif ()
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES sunshine_http)
