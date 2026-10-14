# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_WEBHOOK_SOURCES
    webhook_auth.cpp
    webhook_api.cpp
    webhook_format.cpp
    webhook.cpp
)
list(TRANSFORM SUNSHINE_WEBHOOK_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_WEBHOOK_HEADERS
    webhook_auth.h
    webhook_api.h
    webhook_format.h
    webhook.h
)
list(TRANSFORM SUNSHINE_WEBHOOK_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
