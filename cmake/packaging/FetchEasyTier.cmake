# FetchEasyTier.cmake — download the pinned EasyTier host runtime.
#
# The archive is fetched from the official EasyTier release, verified before
# extraction, and installed as a private Sunshine runtime under tools/easytier.

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

option(FETCH_EASYTIER "Download the EasyTier runtime used by Remote Connect" ON)
option(EASYTIER_REQUIRED "Treat a missing EasyTier runtime as a configuration error" ON)
set(EASYTIER_VERSION "v2.6.4")
set(EASYTIER_CACHE_DIR "${CMAKE_BINARY_DIR}/_easytier" CACHE PATH "EasyTier runtime cache")

if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
  set(_EASYTIER_ARCH "arm64")
  set(_EASYTIER_SHA256 "37023f8a3451c9234b17ee2089a03dc344ce90d803b5b359cb6c46682b0549b4")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(_EASYTIER_ARCH "i686")
  set(_EASYTIER_SHA256 "bf557daeccc5525d95b8a230c339d75554fb52d82d1e050e9a5202c92c02e09e")
else()
  set(_EASYTIER_ARCH "x86_64")
  set(_EASYTIER_SHA256 "27af91e270e554709b048bd32327fefd2dfce5062ae1e8701af7550c6f525f84")
endif()

set(_EASYTIER_BASENAME "easytier-windows-${_EASYTIER_ARCH}-${EASYTIER_VERSION}")
set(_EASYTIER_ARCHIVE "${EASYTIER_CACHE_DIR}/${_EASYTIER_BASENAME}.zip")
set(EASYTIER_RUNTIME_DIR "${EASYTIER_CACHE_DIR}/easytier-windows-${_EASYTIER_ARCH}"
    CACHE PATH "Extracted EasyTier runtime directory" FORCE)
set(EASYTIER_LICENSE "${EASYTIER_CACHE_DIR}/LICENSE-EasyTier.txt"
    CACHE FILEPATH "EasyTier license file" FORCE)

function(_easytier_download url output expected_sha256)
  if(EXISTS "${output}")
    file(SHA256 "${output}" _cached_sha256)
    if(_cached_sha256 STREQUAL expected_sha256)
      return()
    endif()
    message(WARNING "Cached EasyTier artifact failed verification; downloading it again")
    file(REMOVE "${output}")
  endif()

  get_filename_component(_output_dir "${output}" DIRECTORY)
  file(MAKE_DIRECTORY "${_output_dir}")
  message(STATUS "Downloading pinned EasyTier artifact: ${url}")
  find_program(_EASYTIER_CURL curl)
  if(_EASYTIER_CURL)
    execute_process(
      COMMAND "${_EASYTIER_CURL}" -fsSL --retry 3 -o "${output}" "${url}"
      RESULT_VARIABLE _code
      ERROR_VARIABLE _message)
  else()
    file(DOWNLOAD "${url}" "${output}" STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    list(GET _status 1 _message)
  endif()
  if(NOT _code EQUAL 0)
    file(REMOVE "${output}")
    message(WARNING "EasyTier download failed (${_code}): ${_message}")
    return()
  endif()

  file(SHA256 "${output}" _actual_sha256)
  if(NOT _actual_sha256 STREQUAL expected_sha256)
    file(REMOVE "${output}")
    message(WARNING
      "EasyTier artifact SHA-256 mismatch\n"
      "expected: ${expected_sha256}\n"
      "actual:   ${_actual_sha256}")
  endif()
endfunction()

set(_EASYTIER_REQUIRED_FILES
    easytier-core.exe
    Packet.dll
    WinDivert64.sys
    wintun.dll)

if(FETCH_EASYTIER)
  _easytier_download(
    "https://github.com/EasyTier/EasyTier/releases/download/${EASYTIER_VERSION}/${_EASYTIER_BASENAME}.zip"
    "${_EASYTIER_ARCHIVE}"
    "${_EASYTIER_SHA256}")
  _easytier_download(
    "https://raw.githubusercontent.com/EasyTier/EasyTier/${EASYTIER_VERSION}/LICENSE"
    "${EASYTIER_LICENSE}"
    "e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118")

  if(EXISTS "${_EASYTIER_ARCHIVE}")
    set(_runtime_complete TRUE)
    foreach(_file IN LISTS _EASYTIER_REQUIRED_FILES)
      if(NOT EXISTS "${EASYTIER_RUNTIME_DIR}/${_file}")
        set(_runtime_complete FALSE)
      endif()
    endforeach()
    if(NOT _runtime_complete)
      file(REMOVE_RECURSE "${EASYTIER_RUNTIME_DIR}")
      file(ARCHIVE_EXTRACT INPUT "${_EASYTIER_ARCHIVE}" DESTINATION "${EASYTIER_CACHE_DIR}")
    endif()
  endif()
endif()

set(EASYTIER_AVAILABLE TRUE)
foreach(_file IN LISTS _EASYTIER_REQUIRED_FILES)
  if(NOT EXISTS "${EASYTIER_RUNTIME_DIR}/${_file}")
    set(EASYTIER_AVAILABLE FALSE)
  endif()
endforeach()
if(NOT EXISTS "${EASYTIER_LICENSE}")
  set(EASYTIER_AVAILABLE FALSE)
endif()

if(NOT EASYTIER_AVAILABLE)
  if(EASYTIER_REQUIRED)
    message(FATAL_ERROR
      "The pinned EasyTier ${EASYTIER_VERSION} runtime is unavailable. "
      "Set FETCH_EASYTIER=ON with network access, or provide the verified runtime in ${EASYTIER_RUNTIME_DIR}.")
  endif()
  message(WARNING "EasyTier is unavailable; Remote Connect will not be included in this package")
endif()

set(EASYTIER_AVAILABLE "${EASYTIER_AVAILABLE}" CACHE INTERNAL
    "Whether the verified EasyTier runtime is available" FORCE)
