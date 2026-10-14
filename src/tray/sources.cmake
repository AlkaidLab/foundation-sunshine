# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_TRAY_SOURCES
    system_tray.cpp
    system_tray_i18n.cpp
    tray_http.cpp
    tray_state.cpp
)
list(TRANSFORM SUNSHINE_TRAY_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_TRAY_HEADERS
    system_tray.h
    system_tray_i18n.h
    tray_http.h
    tray_state.h
)
list(TRANSFORM SUNSHINE_TRAY_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
