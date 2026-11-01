cmake_minimum_required(VERSION 3.20)

project(alk_ds5_core LANGUAGES C)

option(DS5_CORE_STANDALONE "Build the DS5 core without Platform dependencies" ON)
option(DS5_BUILD_FUZZER "Build the ASan/UBSan libFuzzer target" OFF)

add_library(ds5_core STATIC src/ds5_core.c)
target_include_directories(ds5_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ds5_core PUBLIC c_std_11)
set_target_properties(ds5_core PROPERTIES C_EXTENSIONS OFF)

if(DS5_BUILD_FUZZER)
  target_compile_options(ds5_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  target_link_options(ds5_core PRIVATE -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  enable_testing()
  add_executable(ds5_core_contract_test tests/ds5_core_contract_test.c)
  target_link_libraries(ds5_core_contract_test PRIVATE ds5_core)
  target_compile_features(ds5_core_contract_test PRIVATE c_std_11)
  target_compile_definitions(ds5_core_contract_test PRIVATE DS5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ds5_core_contract_test COMMAND ds5_core_contract_test)

  add_executable(ds5_core_report_vectors_test tests/ds5_core_report_vectors_test.c)
  target_link_libraries(ds5_core_report_vectors_test PRIVATE ds5_core)
  target_compile_features(ds5_core_report_vectors_test PRIVATE c_std_11)
  target_compile_definitions(ds5_core_report_vectors_test PRIVATE DS5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ds5_core_report_vectors_test COMMAND ds5_core_report_vectors_test)
endif()

if(DS5_BUILD_FUZZER)
  add_executable(ds5_core_fuzz tests/ds5_core_fuzz.c)
  target_link_libraries(ds5_core_fuzz PRIVATE ds5_core)
  target_compile_features(ds5_core_fuzz PRIVATE c_std_11)
  target_compile_options(ds5_core_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(ds5_core_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
