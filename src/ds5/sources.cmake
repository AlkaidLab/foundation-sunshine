# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_DS5_SOURCES
    config.cpp
    config_api.cpp
)
list(TRANSFORM SUNSHINE_DS5_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_DS5_HEADERS
    config.h
    config_api.h
)
list(TRANSFORM SUNSHINE_DS5_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
