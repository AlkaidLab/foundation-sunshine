# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_AI_SOURCES
    credential_store.cpp
    http.cpp
    config_store.cpp
    proxy.cpp
)
list(TRANSFORM SUNSHINE_AI_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_AI_HEADERS
    credential_store.h
    http.h
    http_stream.h
    config_store.h
    provider.h
    proxy.h
)
list(TRANSFORM SUNSHINE_AI_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
