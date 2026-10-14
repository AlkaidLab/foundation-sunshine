# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_PYROWAVE_SOURCES
    types.cpp
    config.cpp
    capabilities.cpp
    gpu_interop.cpp
    packet.cpp
    packetizer.cpp
    dynamic_hdr.cpp
    session.cpp
    runtime.cpp
)
list(TRANSFORM SUNSHINE_PYROWAVE_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_PYROWAVE_HEADERS
    types.h
    config.h
    capabilities.h
    gpu_interop.h
    packet.h
    packetizer.h
    dynamic_hdr.h
    encoder.h
    session.h
    runtime.h
    pyrowave.h
)
list(TRANSFORM SUNSHINE_PYROWAVE_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
