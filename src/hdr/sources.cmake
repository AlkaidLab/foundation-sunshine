# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_HDR_SOURCES
    client_display_capabilities.cpp
    dynamic_hdr_selection.cpp
    session_target.cpp
)
list(TRANSFORM SUNSHINE_HDR_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_HDR_HEADERS
    client_display_capabilities.h
    dynamic_hdr_selection.h
    session_target.h
)
list(TRANSFORM SUNSHINE_HDR_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
