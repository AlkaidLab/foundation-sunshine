# DualSense tuning behaviour matrix: every one of the 106 keys is saved through the settings store and
# its effect measured at the device boundary.  The mirror rig runs on any host; the device rig needs
# the Windows native virtual pad and is run by hand.
add_executable(ds5_tuning_matrix_sim
        ${CMAKE_SOURCE_DIR}/tests/tools/ds5_matrix/ds5_tuning_matrix_sim.cpp
        ${CMAKE_SOURCE_DIR}/src/ds5/tuning_profile.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/legacy_rumble.cpp)
target_include_directories(ds5_tuning_matrix_sim PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ds5_tuning_matrix_sim PRIVATE moonlight::haptics alkaidlab_ds5_core nlohmann_json::nlohmann_json
        sunshine_project_options)
target_compile_definitions(ds5_tuning_matrix_sim PRIVATE DS5_TUNING_TEST_NATIVE_PATH=1)
set_target_properties(ds5_tuning_matrix_sim PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(ds5_tuning_matrix_sim PROPERTIES LINK_SEARCH_START_STATIC 1)

    add_executable(ds5_tuning_matrix_win
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_matrix/ds5_tuning_matrix_win.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/ds5/ds5_native_client.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/ds5/ds5_pad_audio.cpp
            ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
            ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_host_controller.cpp
            ${CMAKE_SOURCE_DIR}/src/remote_usb/usbip_stale_port_sweep.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/text_encoding.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/audio_default_guard.cpp
            ${CMAKE_SOURCE_DIR}/src/ds5/tuning_profile.cpp
            ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir.cpp
            ${CMAKE_SOURCE_DIR}/src/haptics/legacy_rumble.cpp
            ${CMAKE_SOURCE_DIR}/src/crypto.cpp)
    target_include_directories(ds5_tuning_matrix_win PRIVATE
            "${CMAKE_SOURCE_DIR}"
            "${CMAKE_SOURCE_DIR}/third-party"
            ${FFMPEG_INCLUDE_DIRS}
            "${CMAKE_SOURCE_DIR}/third-party/build-deps/dist/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}/include")
    target_link_libraries(ds5_tuning_matrix_win PRIVATE
            sunshine_project_options sunshine_crypto Boost::log nlohmann_json::nlohmann_json moonlight::haptics
            ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES} ${PLATFORM_LIBRARIES}
            alkaidlab_ds5_core alkaidlab_usbip_core setupapi hid ole32 uuid)
    target_compile_options(ds5_tuning_matrix_win PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(ds5_tuning_matrix_win PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
endif ()
