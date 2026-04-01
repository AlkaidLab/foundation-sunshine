# FetchDriverDeps.cmake — Download driver dependencies from GitHub Releases
#
# Downloads pre-built signed driver binaries at configure time.
# All downloads are cached in ${CMAKE_BINARY_DIR}/_driver_deps/.
#
# Configuration (CMake cache variables):
#   FETCH_DRIVER_DEPS       — Enable/disable downloads (default: ON)
#   VMOUSE_DRIVER_VERSION   — ZakoVirtualMouse release tag (e.g. v1.1.0)
#   VDD_DRIVER_VERSION      — ZakoVDD release tag (e.g. v0.1.4)
#   NEFCON_VERSION          — nefcon release tag (e.g. v1.10.0)
#   GITHUB_TOKEN            — Token for private repos (or set env GITHUB_TOKEN)
#
# Output variables (CACHE FORCE, available to parent):
#   VMOUSE_DRIVER_DIR       — Directory containing vmouse driver files
#   VDD_DRIVER_DIR          — Directory containing VDD driver files
#   NEFCON_DRIVER_DIR       — Directory containing nefconw.exe

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

option(FETCH_DRIVER_DEPS "Download driver dependencies from GitHub Releases" ON)

# Version pins
set(VMOUSE_DRIVER_VERSION "v1.2.0" CACHE STRING "ZakoVirtualMouse driver version tag")
set(VDD_DRIVER_VERSION "v0.13" CACHE STRING "ZakoVDD driver version tag")
set(NEFCON_VERSION "v1.10.0" CACHE STRING "nefcon version tag")

# Repositories
set(_VMOUSE_REPO "AlkaidLab/ZakoVirtualMouse")
set(_VDD_REPO "qiin2333/zako-vdd")
set(_NEFCON_REPO "nefarius/nefcon")

# Output directories
set(DRIVER_DEPS_CACHE "${CMAKE_BINARY_DIR}/_driver_deps" CACHE PATH "Driver dependencies cache")
set(VMOUSE_DRIVER_DIR "${DRIVER_DEPS_CACHE}/vmouse" CACHE PATH "" FORCE)
set(VDD_DRIVER_DIR "${DRIVER_DEPS_CACHE}/vdd" CACHE PATH "" FORCE)
set(NEFCON_DRIVER_DIR "${DRIVER_DEPS_CACHE}/nefcon" CACHE PATH "" FORCE)

if(NOT FETCH_DRIVER_DEPS)
  message(STATUS "Driver dependency downloads disabled (FETCH_DRIVER_DEPS=OFF)")
  return()
endif()

# GitHub token for private repos
if(NOT GITHUB_TOKEN AND DEFINED ENV{GITHUB_TOKEN})
  set(GITHUB_TOKEN "$ENV{GITHUB_TOKEN}")
endif()

# ---------------------------------------------------------------------------
# Helper: download a single file (skip if already cached)
# Uses curl for authenticated requests to handle GitHub's 302 redirects
# properly (CMake file(DOWNLOAD) doesn't forward auth headers on redirect).
# ---------------------------------------------------------------------------
function(_driver_download url output_path)
  if(EXISTS "${output_path}")
    return()
  endif()

  get_filename_component(_dir "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")

  message(STATUS "  Downloading: ${url}")

  if(GITHUB_TOKEN)
    # Use curl to handle GitHub's 302 redirects for private repo assets.
    # CMake's file(DOWNLOAD) won't send auth headers after redirect to S3.
    find_program(_CURL curl REQUIRED)
    execute_process(
      COMMAND "${_CURL}" -fsSL
        -H "Authorization: token ${GITHUB_TOKEN}"
        -H "Accept: application/octet-stream"
        -o "${output_path}"
        "${url}"
      RESULT_VARIABLE _code
      ERROR_VARIABLE _err)
    if(NOT _code EQUAL 0)
      message(WARNING "  curl download failed (${_code}): ${_err}")
      file(REMOVE "${output_path}")
      return()
    endif()
  else()
    file(DOWNLOAD "${url}" "${output_path}"
      STATUS _status
      TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _status 1 _msg)
      message(WARNING "  Download failed (${_code}): ${_msg}")
      file(REMOVE "${output_path}")
      return()
    endif()
  endif()

  if(EXISTS "${output_path}")
    file(SIZE "${output_path}" _size)
    if(_size EQUAL 0)
      message(WARNING "  Downloaded file is empty: ${output_path}")
      file(REMOVE "${output_path}")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# ZakoVirtualMouse  (private repo — use GitHub API for authenticated downloads)
# For private repos, browser_download_url returns 302→S3 which rejects
# forwarded auth headers. We must use the GitHub REST API asset endpoint
# with Accept: application/octet-stream.
# ---------------------------------------------------------------------------
function(_fetch_vmouse)
  message(STATUS "Fetching ZakoVirtualMouse ${VMOUSE_DRIVER_VERSION} ...")

  set(_files ZakoVirtualMouse.dll ZakoVirtualMouse.inf ZakoVirtualMouse.cat ZakoVirtualMouse.cer)

  # Check if all files already cached
  set(_all_cached TRUE)
  foreach(_f ${_files})
    if(NOT EXISTS "${VMOUSE_DRIVER_DIR}/${_f}")
      set(_all_cached FALSE)
      break()
    endif()
  endforeach()
  if(_all_cached)
    message(STATUS "  All vmouse files already cached")
    return()
  endif()

  if(NOT GITHUB_TOKEN)
    message(WARNING "  GITHUB_TOKEN required for private repo ${_VMOUSE_REPO}")
    return()
  endif()

  find_program(_CURL curl REQUIRED)
  file(MAKE_DIRECTORY "${VMOUSE_DRIVER_DIR}")

  # Query release assets via GitHub API
  set(_api_url "https://api.github.com/repos/${_VMOUSE_REPO}/releases/tags/${VMOUSE_DRIVER_VERSION}")
  set(_json "${DRIVER_DEPS_CACHE}/_vmouse_release.json")
  execute_process(
    COMMAND "${_CURL}" -fsSL
      -H "Authorization: token ${GITHUB_TOKEN}"
      -H "Accept: application/vnd.github+json"
      -o "${_json}"
      "${_api_url}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(WARNING "  Failed to query vmouse release API (${_rc}): ${_err}")
    return()
  endif()

  # For each required file, find its asset id and download via API
  foreach(_f ${_files})
    if(EXISTS "${VMOUSE_DRIVER_DIR}/${_f}")
      continue()
    endif()

    # Extract asset download URL from JSON using regex
    # The API JSON contains entries like:
    #   "name": "ZakoVirtualMouse.dll", ... "url": "https://api.github.com/repos/.../assets/12345"
    file(READ "${_json}" _json_content)

    # Find block for this asset: locate "name": "<filename>" then extract nearest "url"
    # We use string(REGEX) to find the asset API url
    string(REGEX MATCH "\"url\"[^}]*\"name\":[ ]*\"${_f}\"" _match_after "${_json_content}")
    string(REGEX MATCH "\"name\":[ ]*\"${_f}\"[^}]*\"url\"" _match_before "${_json_content}")

    set(_asset_api_url "")
    # Try to extract the url from the assets array
    # GitHub API returns assets like: { "url": "https://api.github.com/repos/.../assets/ID", ... "name": "file" }
    string(REGEX MATCH "\"url\":[ ]*\"(https://api\\.github\\.com/repos/[^\"]+/assets/[0-9]+)\"[^}]*\"name\":[ ]*\"${_f}\"" _m "${_json_content}")
    if(_m)
      set(_asset_api_url "${CMAKE_MATCH_1}")
    endif()

    if(NOT _asset_api_url)
      message(WARNING "  Could not find asset URL for ${_f} in release JSON")
      continue()
    endif()

    message(STATUS "  Downloading ${_f} via API: ${_asset_api_url}")
    execute_process(
      COMMAND "${_CURL}" -fsSL
        -H "Authorization: token ${GITHUB_TOKEN}"
        -H "Accept: application/octet-stream"
        -o "${VMOUSE_DRIVER_DIR}/${_f}"
        "${_asset_api_url}"
      RESULT_VARIABLE _rc
      ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(WARNING "  Download failed for ${_f} (${_rc}): ${_err}")
      file(REMOVE "${VMOUSE_DRIVER_DIR}/${_f}")
    endif()
  endforeach()

  file(REMOVE "${_json}")
endfunction()

# ---------------------------------------------------------------------------
# ZakoVDD  (single zip release asset)
# ---------------------------------------------------------------------------
function(_fetch_vdd)
  message(STATUS "Fetching ZakoVDD ${VDD_DRIVER_VERSION} ...")
  set(_zip_url "https://github.com/${_VDD_REPO}/releases/download/${VDD_DRIVER_VERSION}/zakovdd.zip")
  set(_zip "${DRIVER_DEPS_CACHE}/zakovdd-${VDD_DRIVER_VERSION}.zip")

  _driver_download("${_zip_url}" "${_zip}")

  if(EXISTS "${_zip}" AND NOT EXISTS "${VDD_DRIVER_DIR}/ZakoVDD.dll")
    file(MAKE_DIRECTORY "${VDD_DRIVER_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${VDD_DRIVER_DIR}")
    message(STATUS "  Extracted VDD driver to ${VDD_DRIVER_DIR}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# nefcon  (zip with architecture subdirectories)
# ---------------------------------------------------------------------------
function(_fetch_nefcon)
  message(STATUS "Fetching nefcon ${NEFCON_VERSION} ...")
  set(_zip_url "https://github.com/${_NEFCON_REPO}/releases/download/${NEFCON_VERSION}/nefcon_${NEFCON_VERSION}.zip")
  set(_zip "${DRIVER_DEPS_CACHE}/nefcon-${NEFCON_VERSION}.zip")

  _driver_download("${_zip_url}" "${_zip}")

  if(EXISTS "${_zip}" AND NOT EXISTS "${NEFCON_DRIVER_DIR}/nefconw.exe")
    set(_tmp "${DRIVER_DEPS_CACHE}/_nefcon_extract")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_tmp}")
    file(MAKE_DIRECTORY "${NEFCON_DRIVER_DIR}")
    file(COPY_FILE "${_tmp}/x64/nefconw.exe" "${NEFCON_DRIVER_DIR}/nefconw.exe")
    file(REMOVE_RECURSE "${_tmp}")
    message(STATUS "  Extracted nefconw.exe (x64) to ${NEFCON_DRIVER_DIR}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Execute all fetches
# ---------------------------------------------------------------------------
_fetch_vmouse()
_fetch_vdd()
_fetch_nefcon()

# ---------------------------------------------------------------------------
# Verify critical files
# ---------------------------------------------------------------------------
set(_missing)
foreach(_f
    "${VMOUSE_DRIVER_DIR}/ZakoVirtualMouse.dll"
    "${VDD_DRIVER_DIR}/ZakoVDD.dll"
    "${NEFCON_DRIVER_DIR}/nefconw.exe")
  if(NOT EXISTS "${_f}")
    list(APPEND _missing "${_f}")
  endif()
endforeach()

if(_missing)
  string(REPLACE ";" "\n  " _list "${_missing}")
  message(FATAL_ERROR
    "Missing driver dependencies:\n  ${_list}\n"
    "For private repos, set -DGITHUB_TOKEN=<token> or env GITHUB_TOKEN.\n"
    "To skip downloads: -DFETCH_DRIVER_DEPS=OFF (provide files manually in ${DRIVER_DEPS_CACHE}).")
endif()
