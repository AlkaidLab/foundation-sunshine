# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_FILE_MAPPING_SOURCES
    file_mapping.cpp
    file_mapping_config.cpp
    file_mapping_http.cpp
    file_mapping_operations.cpp
    file_mapping_rpc.cpp
    file_mapping_store.cpp
    file_mapping_token.cpp
    file_mapping_ws.cpp
    file_mapping_ws_session.cpp
    file_mapping_ws_server.cpp
    service.cpp
    config_http.cpp
)
list(TRANSFORM SUNSHINE_FILE_MAPPING_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_FILE_MAPPING_HEADERS
    file_mapping.h
    file_mapping_base64.h
    file_mapping_config.h
    file_mapping_http.h
    file_mapping_operations.h
    file_mapping_rpc.h
    file_mapping_store.h
    file_mapping_token.h
    file_mapping_ws.h
    file_mapping_ws_session.h
    file_mapping_ws_server.h
    service.h
    config_http.h
)
list(TRANSFORM SUNSHINE_FILE_MAPPING_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
