# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_DISPLAY_DEVICE_SOURCES
    color_profile.cpp
    parsed_config.cpp
    session.cpp
    settings.cpp
    to_string.cpp
    vdd_capability.cpp
    vdd_utils.cpp
    vdd_ioctl.cpp
    vdd_settings.cpp
)
list(TRANSFORM SUNSHINE_DISPLAY_DEVICE_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_DISPLAY_DEVICE_HEADERS
    display_device.h
    color_profile.h
    parsed_config.h
    session.h
    settings.h
    to_string.h
    vdd_capability.h
    vdd_utils.h
    vdd_ioctl.h
    vdd_control_ioctl.h
    vdd_settings.h
)
list(TRANSFORM SUNSHINE_DISPLAY_DEVICE_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
