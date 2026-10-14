# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_IMAGE_ENHANCEMENT_SOURCES
    config.cpp
    nr_defaults.cpp
    api.cpp
)
list(TRANSFORM SUNSHINE_IMAGE_ENHANCEMENT_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_IMAGE_ENHANCEMENT_HEADERS
    config.h
    nr_defaults.h
    api.h
)
list(TRANSFORM SUNSHINE_IMAGE_ENHANCEMENT_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
