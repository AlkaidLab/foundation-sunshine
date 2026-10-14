# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_TEXT_CONTEXT_SOURCES
    bridge.cpp
    http.cpp
)
list(TRANSFORM SUNSHINE_TEXT_CONTEXT_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_TEXT_CONTEXT_HEADERS
    bridge.h
    http.h
)
list(TRANSFORM SUNSHINE_TEXT_CONTEXT_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
