# FetchDriverDeps.cmake — Download driver dependencies from GitHub Releases
#
# Downloads pre-built signed driver binaries at configure time.
# All downloads are cached in ${CMAKE_BINARY_DIR}/_driver_deps/.
#
# Configuration (CMake cache variables):
#   FETCH_DRIVER_DEPS       — Enable/disable downloads (default: ON)
#   DRIVER_DEPS_REQUIRED    — If ON (default), missing driver files are a
#                             FATAL_ERROR. If OFF (typical for fork-PR CI),
#                             missing files become a WARNING and the affected
#                             driver is excluded from packaging.
#   VMOUSE_DRIVER_VERSION   — ZakoVirtualMouse release tag (e.g. v1.1.0)
#   VMOUSE_PUBLIC_REPO      — Public mirror repo for vmouse release assets
#                             (default: AlkaidLab/zako-vmouse-release). Tried
#                             first; falls back to private repo via API if
#                             GITHUB_TOKEN is available.
#   VDD_DRIVER_VERSION      — ZakoVDD release tag (e.g. v0.1.4)
#   VDD_WIN10_DRIVER_VERSION — Win10-pinned ZakoVDD release tag
#   NEFCON_VERSION          — nefcon release tag (e.g. v1.10.0)
#   VIGEMBUS_VERSION        — pinned ViGEmBus release tag
#   VIGEMBUS_ASSET_NAME     — pinned multi-architecture installer asset
#   VIGEMBUS_SHA256         — expected installer digest
#   GITHUB_TOKEN            — Token for private repos (or set env GITHUB_TOKEN)
#
# Output variables (CACHE FORCE, available to parent):
#   VMOUSE_DRIVER_DIR       — Directory containing vmouse driver files
#   VDD_DRIVER_DIR          — Directory containing latest VDD driver files
#   VDD_WIN10_DRIVER_DIR    — Directory containing Win10-pinned VDD driver files
#   NEFCON_DRIVER_DIR       — Directory containing nefconw.exe
#   VIGEMBUS_INSTALLER      — Hash-verified installer bundled into packages

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

option(FETCH_DRIVER_DEPS "Download driver dependencies from GitHub Releases" ON)
option(DRIVER_DEPS_REQUIRED "Treat missing driver dependencies as a fatal error" ON)

# Version pins
set(VMOUSE_DRIVER_VERSION "v1.2.0" CACHE STRING "ZakoVirtualMouse driver version tag")
set(VDD_DRIVER_VERSION "v0.16.6" CACHE STRING "ZakoVDD driver version tag")
set(VDD_WIN10_DRIVER_VERSION "v0.14.3-rc1-edid13-test" CACHE STRING "Win10-pinned ZakoVDD driver version tag")
set(VDD_DRIVER_ASSET_NAME "zakovdd.zip" CACHE STRING "Latest ZakoVDD release asset name")
set(VDD_WIN10_DRIVER_ASSET_NAME "ZakoVDD-edid13-issue612.zip" CACHE STRING "Win10-pinned ZakoVDD release asset name")
set(NEFCON_VERSION "v1.17.40" CACHE STRING "nefcon version tag")
set(VIGEMBUS_VERSION "v1.22.0" CACHE STRING "ViGEmBus release tag")
set(VIGEMBUS_ASSET_NAME "ViGEmBus_1.22.0_x64_x86_arm64.exe"
    CACHE STRING "ViGEmBus release asset name")
set(VIGEMBUS_SHA256 "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a"
    CACHE STRING "SHA256 of the pinned ViGEmBus installer")

# Repositories
set(_VMOUSE_REPO "AlkaidLab/ZakoVirtualMouse")
set(VMOUSE_PUBLIC_REPO "AlkaidLab/zako-vmouse-release" CACHE STRING
    "Public mirror repo (owner/name) hosting ZakoVirtualMouse release assets")
set(_VDD_REPO "qiin2333/zako-vdd")
set(_NEFCON_REPO "nefarius/nefcon")

# Output directories
set(DRIVER_DEPS_CACHE "${CMAKE_BINARY_DIR}/_driver_deps" CACHE PATH "Driver dependencies cache")
set(VMOUSE_DRIVER_DIR "${DRIVER_DEPS_CACHE}/vmouse" CACHE PATH "" FORCE)
set(VDD_DRIVER_DIR "${DRIVER_DEPS_CACHE}/vdd" CACHE PATH "" FORCE)
set(VDD_WIN10_DRIVER_DIR "${DRIVER_DEPS_CACHE}/vdd-win10" CACHE PATH "" FORCE)
set(NEFCON_DRIVER_DIR "${DRIVER_DEPS_CACHE}/nefcon" CACHE PATH "" FORCE)
set(VIGEMBUS_DIR "${DRIVER_DEPS_CACHE}/vigembus" CACHE PATH "" FORCE)
set(VIGEMBUS_INSTALLER "${VIGEMBUS_DIR}/${VIGEMBUS_ASSET_NAME}"
    CACHE FILEPATH "Pinned ViGEmBus installer" FORCE)

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
  set(_expected_sha256 "")
  if(ARGC GREATER 2)
    set(_expected_sha256 "${ARGV2}")
  endif()

  if(EXISTS "${output_path}")
    if(_expected_sha256)
      file(SHA256 "${output_path}" _cached_sha256)
      if(_cached_sha256 STREQUAL _expected_sha256)
        return()
      endif()
      message(WARNING "  Cached file hash mismatch; downloading again: ${output_path}")
      file(REMOVE "${output_path}")
    else()
      return()
    endif()
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

  if(EXISTS "${output_path}" AND _expected_sha256)
    file(SHA256 "${output_path}" _actual_sha256)
    if(NOT _actual_sha256 STREQUAL _expected_sha256)
      message(WARNING
        "  SHA256 mismatch for ${output_path}\n"
        "  expected: ${_expected_sha256}\n"
        "  actual:   ${_actual_sha256}")
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

  file(MAKE_DIRECTORY "${VMOUSE_DRIVER_DIR}")

  # ---- Attempt 1: public mirror repo (no auth needed) ----
  if(VMOUSE_PUBLIC_REPO)
    message(STATUS "  Trying public mirror ${VMOUSE_PUBLIC_REPO} ...")
    foreach(_f ${_files})
      if(EXISTS "${VMOUSE_DRIVER_DIR}/${_f}")
        continue()
      endif()
      set(_url "https://github.com/${VMOUSE_PUBLIC_REPO}/releases/download/${VMOUSE_DRIVER_VERSION}/${_f}")
      _driver_download("${_url}" "${VMOUSE_DRIVER_DIR}/${_f}")
    endforeach()

    # If all files now present, we're done.
    set(_all_ok TRUE)
    foreach(_f ${_files})
      if(NOT EXISTS "${VMOUSE_DRIVER_DIR}/${_f}")
        set(_all_ok FALSE)
        break()
      endif()
    endforeach()
    if(_all_ok)
      message(STATUS "  vmouse fetched from public mirror")
      return()
    endif()
  endif()

  # ---- Attempt 2: private repo via GitHub API (requires token) ----
  if(NOT GITHUB_TOKEN)
    message(WARNING
      "  vmouse not available from public mirror '${VMOUSE_PUBLIC_REPO}' "
      "at tag ${VMOUSE_DRIVER_VERSION}, and GITHUB_TOKEN is not set to fall "
      "back on private repo ${_VMOUSE_REPO}.")
    return()
  endif()

  find_program(_CURL curl REQUIRED)

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
function(_fetch_vdd_release variant_label version asset_name output_dir cache_prefix)
  message(STATUS "Fetching ZakoVDD ${variant_label} ${version} ...")
  set(_zip_url "https://github.com/${_VDD_REPO}/releases/download/${version}/${asset_name}")
  set(_zip "${DRIVER_DEPS_CACHE}/${cache_prefix}-${version}.zip")
  set(_version_marker "${output_dir}/.release-version")
  set(_expected_marker "${version}|${asset_name}")

  _driver_download("${_zip_url}" "${_zip}")

  if(EXISTS "${_zip}")
    set(_needs_extract FALSE)
    if(NOT EXISTS "${output_dir}/ZakoVDD.dll")
      set(_needs_extract TRUE)
    elseif(NOT EXISTS "${_version_marker}")
      set(_needs_extract TRUE)
    else()
      file(READ "${_version_marker}" _current_marker)
      string(STRIP "${_current_marker}" _current_marker)
      if(NOT _current_marker STREQUAL _expected_marker)
        set(_needs_extract TRUE)
      endif()
    endif()
  endif()

  if(_needs_extract)
    file(REMOVE_RECURSE "${output_dir}")
    file(MAKE_DIRECTORY "${output_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${output_dir}")
    file(WRITE "${_version_marker}" "${_expected_marker}\n")
    message(STATUS "  Extracted ${variant_label} VDD driver to ${output_dir}")
  endif()
endfunction()

function(_fetch_vdd)
  _fetch_vdd_release("latest" "${VDD_DRIVER_VERSION}" "${VDD_DRIVER_ASSET_NAME}" "${VDD_DRIVER_DIR}" "zakovdd")
  _fetch_vdd_release("win10" "${VDD_WIN10_DRIVER_VERSION}" "${VDD_WIN10_DRIVER_ASSET_NAME}" "${VDD_WIN10_DRIVER_DIR}" "zakovdd-win10")
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

# Pinned installer bundled into the package so installation never downloads it.
function(_fetch_vigembus)
  set(_url
    "https://github.com/nefarius/ViGEmBus/releases/download/${VIGEMBUS_VERSION}/${VIGEMBUS_ASSET_NAME}")
  _driver_download("${_url}" "${VIGEMBUS_INSTALLER}" "${VIGEMBUS_SHA256}")
endfunction()

# ---------------------------------------------------------------------------
# Execute all fetches
# ---------------------------------------------------------------------------
_fetch_vmouse()
_fetch_vdd()
_fetch_nefcon()
_fetch_vigembus()

# ---------------------------------------------------------------------------
# Verify critical files (per-driver, so optional drivers can be skipped
# individually when DRIVER_DEPS_REQUIRED=OFF, e.g. fork-PR CI).
# ---------------------------------------------------------------------------
function(_check_driver name available_var)
  set(_missing)
  foreach(_f ${ARGN})
    if(NOT EXISTS "${_f}")
      list(APPEND _missing "${_f}")
    endif()
  endforeach()
  if(_missing)
    string(REPLACE ";" "\n  " _list "${_missing}")
    if(DRIVER_DEPS_REQUIRED)
      message(FATAL_ERROR
        "Missing ${name} driver dependencies:\n  ${_list}\n"
        "For private repos, set -DGITHUB_TOKEN=<token> or env GITHUB_TOKEN.\n"
        "To skip downloads: -DFETCH_DRIVER_DEPS=OFF (provide files manually in ${DRIVER_DEPS_CACHE}).\n"
        "To make missing drivers non-fatal (e.g. for fork-PR CI): -DDRIVER_DEPS_REQUIRED=OFF.")
    else()
      message(WARNING
        "Missing ${name} driver dependencies (packaging will skip this driver):\n  ${_list}")
    endif()
    set(${available_var} FALSE CACHE INTERNAL "" FORCE)
  else()
    set(${available_var} TRUE CACHE INTERNAL "" FORCE)
  endif()
endfunction()

_check_driver("vmouse" VMOUSE_DRIVER_AVAILABLE
    "${VMOUSE_DRIVER_DIR}/ZakoVirtualMouse.dll"
    "${VMOUSE_DRIVER_DIR}/ZakoVirtualMouse.inf"
    "${VMOUSE_DRIVER_DIR}/ZakoVirtualMouse.cat"
    "${VMOUSE_DRIVER_DIR}/ZakoVirtualMouse.cer")
_check_driver("vdd (latest)" VDD_DRIVER_AVAILABLE
    "${VDD_DRIVER_DIR}/ZakoVDD.dll"
    "${VDD_DRIVER_DIR}/ZakoVDD.inf"
    "${VDD_DRIVER_DIR}/ZakoVDD.cat"
    "${VDD_DRIVER_DIR}/ZakoVDD.cer")
_check_driver("vdd (win10)" VDD_WIN10_DRIVER_AVAILABLE
    "${VDD_WIN10_DRIVER_DIR}/ZakoVDD.dll"
    "${VDD_WIN10_DRIVER_DIR}/ZakoVDD.inf"
    "${VDD_WIN10_DRIVER_DIR}/ZakoVDD.cat"
    "${VDD_WIN10_DRIVER_DIR}/ZakoVDD.cer")
_check_driver("nefcon" NEFCON_DRIVER_AVAILABLE
    "${NEFCON_DRIVER_DIR}/nefconw.exe")
_check_driver("ViGEmBus" VIGEMBUS_AVAILABLE
    "${VIGEMBUS_INSTALLER}")
