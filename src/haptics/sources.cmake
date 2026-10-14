# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_HAPTICS_SOURCES
    authored_ir.cpp
)
list(TRANSFORM SUNSHINE_HAPTICS_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_HAPTICS_HEADERS
    authored_ir.h
)
list(TRANSFORM SUNSHINE_HAPTICS_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
