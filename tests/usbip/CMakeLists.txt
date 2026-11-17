# Explicitly invoked by read-only machine acceptance; ordinary CI has no USB/IP driver.
add_executable(usbip_external_readonly_probe
        ${CMAKE_SOURCE_DIR}/tests/usbip/external_readonly_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/doctor/doctor_os.cpp)
target_include_directories(usbip_external_readonly_probe PRIVATE "${CMAKE_SOURCE_DIR}")
set_target_properties(usbip_external_readonly_probe PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_link_libraries(usbip_external_readonly_probe PRIVATE ole32 uuid mmdevapi strmiids setupapi advapi32)
    target_compile_definitions(usbip_external_readonly_probe PRIVATE INITGUID)
endif ()
