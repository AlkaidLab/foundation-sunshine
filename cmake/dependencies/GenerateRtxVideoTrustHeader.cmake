if (NOT DEFINED ADAPTER_PATH OR NOT EXISTS "${ADAPTER_PATH}")
  message(FATAL_ERROR "RTX Video adapter DLL is missing: ${ADAPTER_PATH}")
endif ()
if (NOT DEFINED RUNTIME_PATH OR NOT EXISTS "${RUNTIME_PATH}")
  message(FATAL_ERROR "RTX Video runtime DLL is missing: ${RUNTIME_PATH}")
endif ()
if (NOT DEFINED OUTPUT_PATH OR OUTPUT_PATH STREQUAL "")
  message(FATAL_ERROR "OUTPUT_PATH is required")
endif ()

file(SHA256 "${ADAPTER_PATH}" ADAPTER_SHA256)
file(SHA256 "${RUNTIME_PATH}" RUNTIME_SHA256)
set(_content
"#pragma once
#define SUNSHINE_RTX_VIDEO_ADAPTER_SHA256 \"${ADAPTER_SHA256}\"
#define SUNSHINE_RTX_VIDEO_RUNTIME_SHA256 \"${RUNTIME_SHA256}\"
")
set(_temporary "${OUTPUT_PATH}.tmp")
file(WRITE "${_temporary}" "${_content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_temporary}" "${OUTPUT_PATH}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_temporary}")
