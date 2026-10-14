# Explicit host files; shared-library implementations are declared on their owners.
set(SUNSHINE_THIRD_PARTY_SOURCES
    moonlight-common-c/src/RtspParser.c
    nanors/deps/obl/oblas_common.c
    nanors/deps/obl/oblas_lite.c
    nanors/rs.c
    moonlight-common-c/src/PyrowaveProtocol.c
)
list(TRANSFORM SUNSHINE_THIRD_PARTY_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/third-party/")

set(SUNSHINE_THIRD_PARTY_HEADERS
    moonlight-common-c/src/Input.h
    moonlight-common-c/src/Rtsp.h
    moonlight-common-c/src/Video.h
    tray/src/tray.h
    moonlight-common-c/src/PyrowaveProtocol.h
)
list(TRANSFORM SUNSHINE_THIRD_PARTY_HEADERS PREPEND "${CMAKE_SOURCE_DIR}/third-party/")
