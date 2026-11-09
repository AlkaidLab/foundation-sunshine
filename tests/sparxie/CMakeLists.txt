cmake_minimum_required(VERSION 3.16)
project(sparxie_host_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
set(SPX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_executable(sparxie_unit_tests
  ${SPX_ROOT}/src/sparxie/model.cpp
  ${SPX_ROOT}/src/sparxie/stream_display.cpp
  ${SPX_ROOT}/src/sparxie/region.cpp
  ${SPX_ROOT}/src/sparxie/vendors.cpp
  ${SPX_ROOT}/src/sparxie/storefront.cpp
  ${SPX_ROOT}/src/sparxie/scan.cpp
  ${SPX_ROOT}/src/sparxie/store.cpp
  ${SPX_ROOT}/src/sparxie/ini_settings.cpp
  ${SPX_ROOT}/third-party/puff/puff.c
  ${CMAKE_CURRENT_LIST_DIR}/test_store.cpp
  ${CMAKE_CURRENT_LIST_DIR}/test_sparxie_contract.cpp
target_compile_definitions(sparxie_unit_tests PRIVATE SPARXIE_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")
target_include_directories(sparxie_unit_tests PRIVATE ${SPX_ROOT})
target_link_libraries(sparxie_unit_tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads)
include(GoogleTest)
gtest_discover_tests(sparxie_unit_tests)
if(WIN32)
  target_link_libraries(sparxie_unit_tests PRIVATE winhttp windowscodecs ole32 shell32 uuid advapi32)
endif()

add_executable(sparxie_real_install
  ${SPX_ROOT}/src/sparxie/model.cpp
  ${SPX_ROOT}/src/sparxie/stream_display.cpp
  ${SPX_ROOT}/src/sparxie/region.cpp
  ${SPX_ROOT}/src/sparxie/vendors.cpp
  ${SPX_ROOT}/src/sparxie/storefront.cpp
  ${SPX_ROOT}/src/sparxie/scan.cpp
  ${SPX_ROOT}/src/sparxie/store.cpp
  ${SPX_ROOT}/src/sparxie/ini_settings.cpp
  ${SPX_ROOT}/third-party/puff/puff.c
target_include_directories(sparxie_real_install PRIVATE ${SPX_ROOT})
target_link_libraries(sparxie_real_install PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(sparxie_real_install PRIVATE winhttp windowscodecs ole32 shell32 uuid advapi32)
endif()

add_executable(sparxie_ui_fixtures
  ${SPX_ROOT}/src/sparxie/model.cpp
  ${SPX_ROOT}/src/sparxie/stream_display.cpp
  ${SPX_ROOT}/src/sparxie/region.cpp
  ${SPX_ROOT}/src/sparxie/vendors.cpp
  ${SPX_ROOT}/src/sparxie/storefront.cpp
  ${SPX_ROOT}/src/sparxie/scan.cpp
  ${SPX_ROOT}/src/sparxie/store.cpp
  ${SPX_ROOT}/src/sparxie/ini_settings.cpp
  ${SPX_ROOT}/third-party/puff/puff.c
target_include_directories(sparxie_ui_fixtures PRIVATE ${SPX_ROOT})
target_link_libraries(sparxie_ui_fixtures PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(sparxie_ui_fixtures PRIVATE winhttp windowscodecs ole32 shell32 uuid advapi32)
endif()
