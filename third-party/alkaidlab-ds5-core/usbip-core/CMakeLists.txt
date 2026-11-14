cmake_minimum_required(VERSION 3.20)

project(alk_usbip_core LANGUAGES C)

option(USBIP_CORE_STANDALONE "Build the USB/IP core without Platform dependencies" ON)
option(USBIP_BUILD_FUZZER "Build the ASan/UBSan libFuzzer target" OFF)

add_library(usbip_core STATIC src/usbip_core.c)
target_include_directories(usbip_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(usbip_core PUBLIC c_std_11)
set_target_properties(usbip_core PROPERTIES C_EXTENSIONS OFF)

if(USBIP_BUILD_FUZZER)
  target_compile_options(usbip_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  target_link_options(usbip_core PRIVATE -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  enable_testing()
  add_executable(usbip_core_contract_test tests/usbip_core_contract_test.c)
  target_link_libraries(usbip_core_contract_test PRIVATE usbip_core)
  target_compile_features(usbip_core_contract_test PRIVATE c_std_11)
  add_test(NAME usbip_core_contract_test COMMAND usbip_core_contract_test)

  add_executable(usbip_core_negative_test tests/usbip_core_negative_test.c)
  target_link_libraries(usbip_core_negative_test PRIVATE usbip_core)
  target_compile_features(usbip_core_negative_test PRIVATE c_std_11)
  add_test(NAME usbip_core_negative_test COMMAND usbip_core_negative_test)
endif()

if(USBIP_BUILD_FUZZER)
  add_executable(usbip_core_fuzz tests/usbip_core_fuzz.c)
  target_link_libraries(usbip_core_fuzz PRIVATE usbip_core)
  target_compile_features(usbip_core_fuzz PRIVATE c_std_11)
  target_compile_options(usbip_core_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(usbip_core_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
