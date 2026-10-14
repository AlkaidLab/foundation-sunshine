# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_PLATFORM_COMMON_SOURCES
    frame_contract.cpp
)
list(TRANSFORM SUNSHINE_PLATFORM_COMMON_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_PLATFORM_COMMON_HEADERS
    common.h
    frame_contract.h
)
list(TRANSFORM SUNSHINE_PLATFORM_COMMON_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
