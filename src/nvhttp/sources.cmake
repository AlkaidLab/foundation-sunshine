# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_NVHTTP_SOURCES
    abr_api.cpp
    ai_api.cpp
    apps.cpp
    clipboard_api.cpp
    display_control.cpp
    display_scale.cpp
    dynamic_params.cpp
    network_probe.cpp
    network_probe_limiter.cpp
    pairing.cpp
    sessions.cpp
)
list(TRANSFORM SUNSHINE_NVHTTP_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_NVHTTP_HEADERS
    abr_api.h
    ai_api.h
    apps.h
    clipboard_api.h
    display_control.h
    display_scale.h
    dynamic_params.h
    network_probe.h
    network_probe_limiter.h
    pairing.h
    sessions.h
)
list(TRANSFORM SUNSHINE_NVHTTP_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
