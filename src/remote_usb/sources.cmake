# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_REMOTE_USB_SOURCES
    loopback_usbip_bridge.cpp
    reverse_tunnel_service.cpp
    remote_usb_host_controller.cpp
)
list(TRANSFORM SUNSHINE_REMOTE_USB_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_REMOTE_USB_HEADERS
    loopback_usbip_bridge.h
    usbip_endpoint.h
    reverse_tunnel_service.h
    remote_usb_host_controller.h
)
list(TRANSFORM SUNSHINE_REMOTE_USB_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
