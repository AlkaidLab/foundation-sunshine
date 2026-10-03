cmake_minimum_required(VERSION 3.16)

get_filename_component(UDP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sunshine_platform_udp_validation LANGUAGES CXX)
    include(CTest)
    add_subdirectory("${UDP_SOURCE_ROOT}/third-party/googletest" googletest EXCLUDE_FROM_ALL)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Platform UDP validation currently covers the Windows implementation only")
endif()

# Source-tree Boost::headers only exports libs/headers/include. The production
# declarations used here require Asio and Log and their transitive headers.
if(TARGET Boost::asio AND TARGET Boost::log)
    set(UDP_BOOST_HEADER_TARGET Boost::asio Boost::log)
elseif(TARGET Boost::headers)
    set(UDP_BOOST_HEADER_TARGET Boost::headers)
elseif(TARGET Boost::boost)
    set(UDP_BOOST_HEADER_TARGET Boost::boost)
else()
    find_path(UDP_DEPENDENCY_INCLUDE_DIR boost/core/noncopyable.hpp
            HINTS "${UDP_SOURCE_ROOT}/third-party/build-deps/dist/Windows-AMD64/include"
            REQUIRED)
endif()
find_path(UDP_FFMPEG_INCLUDE_DIR libavutil/pixfmt.h
        HINTS ${FFMPEG_INCLUDE_DIRS} "${FFMPEG_PREPARED_BINARIES}/include"
            "${UDP_SOURCE_ROOT}/third-party/build-deps/dist/Windows-AMD64/include"
        REQUIRED)
add_executable(platform_udp_send_tests "${UDP_SOURCE_ROOT}/tests/unit/test_platform_udp_send.cpp")
target_include_directories(platform_udp_send_tests PRIVATE "${UDP_SOURCE_ROOT}" "${UDP_SOURCE_ROOT}/third-party")
target_include_directories(platform_udp_send_tests SYSTEM PRIVATE "${UDP_FFMPEG_INCLUDE_DIR}")
if(UDP_BOOST_HEADER_TARGET)
    target_link_libraries(platform_udp_send_tests PRIVATE ${UDP_BOOST_HEADER_TARGET})
else()
    target_include_directories(platform_udp_send_tests SYSTEM PRIVATE "${UDP_DEPENDENCY_INCLUDE_DIR}")
endif()
target_link_libraries(platform_udp_send_tests PRIVATE gtest_main ws2_32 mswsock)
set_target_properties(platform_udp_send_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(platform_udp_send_tests PRIVATE /W4 /WX)
else()
    target_compile_options(platform_udp_send_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME platform_udp_send_tests COMMAND platform_udp_send_tests)

if(NOT TARGET enet)
    set(ENET_NO_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory("${UDP_SOURCE_ROOT}/third-party/moonlight-common-c/enet" enet EXCLUDE_FROM_ALL)
endif()
add_executable(enet_send_admission_tests "${UDP_SOURCE_ROOT}/tests/unit/test_enet_send_admission.cpp")
target_link_libraries(enet_send_admission_tests PRIVATE gtest_main enet)
set_target_properties(enet_send_admission_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME enet_send_admission_tests COMMAND enet_send_admission_tests)
