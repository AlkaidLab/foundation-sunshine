# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_CONFIGHTTP_SOURCES
    common.cpp
    auth.cpp
    assets.cpp
    apps.cpp
    configuration.cpp
    clients.cpp
    diagnostics.cpp
    runtime.cpp
    integrations.cpp
    proxies.cpp
    routes.cpp
)
list(TRANSFORM SUNSHINE_CONFIGHTTP_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_CONFIGHTTP_HEADERS
    types.h
    common.h
    auth.h
    assets.h
    apps.h
    configuration.h
    clients.h
    diagnostics.h
    runtime.h
    integrations.h
    proxies.h
    asset_paths.h
    routes.h
)
list(TRANSFORM SUNSHINE_CONFIGHTTP_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
