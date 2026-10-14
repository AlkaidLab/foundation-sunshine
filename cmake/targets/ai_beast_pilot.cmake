# The opt-in loopback harness exercises the same HTTP/SSE transport as Sunshine.
option(SUNSHINE_AI_BEAST_PILOT "Build the independent loopback Beast AI/SSE pilot" OFF)
if (NOT SUNSHINE_AI_BEAST_PILOT)
    return()
endif ()

add_library(ai_beast_transport STATIC
        ${CMAKE_SOURCE_DIR}/src/ai/beast_server.cpp
        ${CMAKE_SOURCE_DIR}/src/ai/http_stream.cpp)
target_include_directories(ai_beast_transport PUBLIC "${CMAKE_SOURCE_DIR}" ${OPENSSL_INCLUDE_DIR})
target_link_libraries(ai_beast_transport PUBLIC sunshine_http)
set_target_properties(ai_beast_transport PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_compile_definitions(ai_beast_transport PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ai_beast_transport PUBLIC ws2_32)
endif ()

add_executable(ai_beast_pilot
        ${CMAKE_SOURCE_DIR}/tests/tools/ai_beast_pilot.cpp
        ${CMAKE_SOURCE_DIR}/src/ai/proxy.cpp
        ${CMAKE_SOURCE_DIR}/src/ai/provider.cpp
        ${CMAKE_SOURCE_DIR}/src/ai/credential_store.cpp
        ${CMAKE_SOURCE_DIR}/src/http_json.cpp)
target_link_libraries(ai_beast_pilot PRIVATE ai_beast_transport ${SUNSHINE_EXTERNAL_LIBRARIES})
target_compile_definitions(ai_beast_pilot PRIVATE ${SUNSHINE_DEFINITIONS})
set_target_properties(ai_beast_pilot PROPERTIES CXX_STANDARD 23)

if (BUILD_TESTS)
    add_executable(ai_beast_pilot_tests
            ${CMAKE_SOURCE_DIR}/tests/tools/test_http_safety.cpp
            ${CMAKE_SOURCE_DIR}/src/ai/proxy.cpp
            ${CMAKE_SOURCE_DIR}/src/ai/provider.cpp
            ${CMAKE_SOURCE_DIR}/src/crypto.cpp)
    target_link_libraries(ai_beast_pilot_tests PRIVATE ai_beast_transport ${SUNSHINE_EXTERNAL_LIBRARIES} gtest_main)
    target_compile_definitions(ai_beast_pilot_tests PRIVATE ${SUNSHINE_DEFINITIONS} SUNSHINE_AI_BEAST_PILOT_TESTS)
    set_target_properties(ai_beast_pilot_tests PROPERTIES CXX_STANDARD 23)
    add_test(NAME ai_beast_pilot_tests COMMAND ai_beast_pilot_tests)
    set_tests_properties(ai_beast_pilot_tests PROPERTIES TIMEOUT 60)
endif ()
if (WIN32)
    set_target_properties(ai_beast_pilot PROPERTIES LINK_SEARCH_START_STATIC 1)
    if (BUILD_TESTS)
        set_target_properties(ai_beast_pilot_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    endif ()
endif ()
