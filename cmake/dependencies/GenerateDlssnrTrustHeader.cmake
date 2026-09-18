if (NOT DEFINED ADAPTER_PATH OR NOT EXISTS "${ADAPTER_PATH}")
  message(FATAL_ERROR "DLSS NR adapter DLL is missing: ${ADAPTER_PATH}")
endif ()
if (NOT DEFINED OUTPUT_PATH OR OUTPUT_PATH STREQUAL "")
  message(FATAL_ERROR "OUTPUT_PATH is required")
endif ()

# Only the adapter is pinned at build time. The signed nvngx_dlssnr.dll is
# supplied outside the distribution, so its digest is pinned from the
# persisted component settings and verified at load time.
file(SHA256 "${ADAPTER_PATH}" ADAPTER_SHA256)
set(_content
"#pragma once
#define SUNSHINE_DLSSNR_ADAPTER_SHA256 \"${ADAPTER_SHA256}\"
")
set(_temporary "${OUTPUT_PATH}.tmp")
file(WRITE "${_temporary}" "${_content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_temporary}" "${OUTPUT_PATH}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${_temporary}")
