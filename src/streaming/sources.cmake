# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_STREAMING_SOURCES
    abr.cpp
    bitrate.cpp
    fec.cpp
    perf_recorder.cpp
)
list(TRANSFORM SUNSHINE_STREAMING_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

set(SUNSHINE_STREAMING_HEADERS
    abr.h
    bitrate.h
    fec.h
    perf_recorder.h
    frame_rate_tracker.h
)
list(TRANSFORM SUNSHINE_STREAMING_HEADERS PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
