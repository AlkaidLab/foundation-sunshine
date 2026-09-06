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
#   VMOUSE_PUBLIC_REPO      — Public repo hosting vmouse release assets
#                             and their GitHub SHA-256 digests
#                             (default: AlkaidLab/zako-vmouse-release)
#   VDD_DRIVER_VERSION      — ZakoVDD release tag (e.g. v0.1.4)
#   VDD_WIN10_DRIVER_VERSION — Win10-pinned ZakoVDD release tag
#   NEFCON_VERSION          — nefcon release tag (e.g. v1.10.0)
#   NEFCON_SHA256           — expected SHA-256 for a custom nefcon release;
#                             known pinned versions use their built-in digest
#   VIGEMBUS_VERSION        — pinned ViGEmBus release tag
#   VIGEMBUS_ASSET_NAME     — pinned multi-architecture installer asset
#   VIGEMBUS_SHA256         — expected installer digest
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
set(VMOUSE_DRIVER_VERSION "v1.3.2" CACHE STRING "ZakoVirtualMouse driver version tag")
set(VDD_DRIVER_VERSION "v0.17.2" CACHE STRING "ZakoVDD driver version tag")
set(VDD_WIN10_DRIVER_VERSION "v0.15.8" CACHE STRING "Win10-pinned ZakoVDD driver version tag")
set(VDD_DRIVER_ASSET_NAME "zakovdd.zip" CACHE STRING "Latest ZakoVDD release asset name")
set(VDD_WIN10_DRIVER_ASSET_NAME "zakovdd.zip" CACHE STRING "Win10-pinned ZakoVDD release asset name")
set(NEFCON_VERSION "v1.18.74" CACHE STRING "nefcon version tag")
set(NEFCON_SHA256 "" CACHE STRING "SHA256 of a custom nefcon release archive")
set(_NEFCON_SHA256_V1_18_74 "625abcdea9e84577d094ab65a8542c9977eb50f2371d216961af01cf4901f172")
set(_NEFCON_SHA256_V1_17_40 "812bae7ed7dfb7d6d2284bc7de2f8ccebc92ed2a0b1ae893c53b337096e50c1a")
if(NEFCON_VERSION STREQUAL "v1.18.74")
  set(_NEFCON_EFFECTIVE_SHA256 "${_NEFCON_SHA256_V1_18_74}")
elseif(NEFCON_VERSION STREQUAL "v1.17.40")
  # Keep existing build directories usable when their cached version predates
  # the v1.18.74 default. A release version must always use its matching digest.
  set(_NEFCON_EFFECTIVE_SHA256 "${_NEFCON_SHA256_V1_17_40}")
else()
  set(_nefcon_sha256_is_stale FALSE)
  if(DEFINED NEFCON_CUSTOM_SHA256_VERSION AND
      NOT "${NEFCON_CUSTOM_SHA256_VERSION}" STREQUAL "${NEFCON_VERSION}" AND
      "${NEFCON_SHA256}" STREQUAL "${NEFCON_CUSTOM_SHA256_VALUE}")
    set(_nefcon_sha256_is_stale TRUE)
  elseif("${NEFCON_SHA256}" STREQUAL "${_NEFCON_SHA256_V1_18_74}" OR
      "${NEFCON_SHA256}" STREQUAL "${_NEFCON_SHA256_V1_17_40}")
    set(_nefcon_sha256_is_stale TRUE)
  endif()
  if(NOT NEFCON_SHA256 OR _nefcon_sha256_is_stale)
    message(FATAL_ERROR
      "NEFCON_SHA256 must be provided for custom NEFCON_VERSION=${NEFCON_VERSION}; cached digests from another version are not reused")
  endif()
  set(_NEFCON_EFFECTIVE_SHA256 "${NEFCON_SHA256}")
  set(NEFCON_CUSTOM_SHA256_VERSION "${NEFCON_VERSION}" CACHE INTERNAL
      "custom nefcon version associated with the cached SHA256" FORCE)
  set(NEFCON_CUSTOM_SHA256_VALUE "${NEFCON_SHA256}" CACHE INTERNAL
      "last accepted custom nefcon SHA256" FORCE)
endif()
set(VIGEMBUS_VERSION "v1.22.0" CACHE STRING "ViGEmBus release tag")
set(VIGEMBUS_ASSET_NAME "ViGEmBus_1.22.0_x64_x86_arm64.exe"
    CACHE STRING "ViGEmBus release asset name")
set(VIGEMBUS_SHA256 "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a"
    CACHE STRING "SHA256 of the pinned ViGEmBus installer")

# Repositories
set(VMOUSE_PUBLIC_REPO "AlkaidLab/zako-vmouse-release" CACHE STRING
    "Public repo (owner/name) hosting ZakoVirtualMouse release assets")
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

set(_DRIVER_DOWNLOAD_ATTEMPTS 3)
set(_DRIVER_DOWNLOAD_TIMEOUT_SECONDS 60)
set(_DRIVER_DOWNLOAD_INACTIVITY_TIMEOUT_SECONDS 15)

if(NOT FETCH_DRIVER_DEPS)
  message(STATUS "Driver dependency downloads disabled (FETCH_DRIVER_DEPS=OFF)")
  return()
endif()

# ---------------------------------------------------------------------------
# Helper: download and optionally verify a file with bounded retries. Partial
# or mismatched downloads are removed between attempts.
# ---------------------------------------------------------------------------
function(_driver_download_with_retries url output_path expected_sha256)
  get_filename_component(_dir "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")

  set(_header_arguments)
  foreach(_header IN LISTS ARGN)
    list(APPEND _header_arguments HTTPHEADER "${_header}")
  endforeach()

  foreach(_attempt RANGE 1 ${_DRIVER_DOWNLOAD_ATTEMPTS})
    file(REMOVE "${output_path}")
    file(DOWNLOAD "${url}" "${output_path}"
      STATUS _status
      TLS_VERIFY ON
      TIMEOUT ${_DRIVER_DOWNLOAD_TIMEOUT_SECONDS}
      INACTIVITY_TIMEOUT ${_DRIVER_DOWNLOAD_INACTIVITY_TIMEOUT_SECONDS}
      ${_header_arguments})

    list(GET _status 0 _code)
    if(_code EQUAL 0 AND EXISTS "${output_path}")
      file(SIZE "${output_path}" _size)
      if(_size GREATER 0)
        if(NOT expected_sha256)
          return()
        endif()

        file(SHA256 "${output_path}" _actual_sha256)
        if(_actual_sha256 STREQUAL expected_sha256)
          return()
        endif()
        set(_status 1 "SHA256 mismatch (expected ${expected_sha256}, actual ${_actual_sha256})")
        set(_code 1)
      else()
        set(_status 1 "downloaded file is empty")
        set(_code 1)
      endif()
    endif()

    list(GET _status 1 _message)
    file(REMOVE "${output_path}")
    if(_attempt LESS _DRIVER_DOWNLOAD_ATTEMPTS)
      message(STATUS
        "  Download attempt ${_attempt}/${_DRIVER_DOWNLOAD_ATTEMPTS} failed (${_code}): ${_message}; retrying")
    else()
      message(WARNING
        "  Download failed after ${_DRIVER_DOWNLOAD_ATTEMPTS} attempts (${_code}): ${_message}")
    endif()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Helper: download a public release asset (skip if a verified cache exists)
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

  message(STATUS "  Downloading: ${url}")
  _driver_download_with_retries("${url}" "${output_path}" "${_expected_sha256}")
endfunction()

function(_download_github_release_metadata repository version output_path)
  if(EXISTS "${output_path}")
    return()
  endif()

  set(_url "https://api.github.com/repos/${repository}/releases/tags/${version}")
  set(_temporary_path "${output_path}.download")
  _driver_download_with_retries(
    "${_url}"
    "${_temporary_path}"
    ""
    "Accept: application/vnd.github+json"
    "X-GitHub-Api-Version: 2022-11-28"
    "User-Agent: Sunshine-Foundation-CMake")

  if(EXISTS "${_temporary_path}")
    file(RENAME "${_temporary_path}" "${output_path}")
  endif()
endfunction()

function(_github_release_asset_sha256 metadata_path asset_name output_variable)
  set(${output_variable} "" PARENT_SCOPE)
  if(NOT EXISTS "${metadata_path}")
    return()
  endif()

  file(READ "${metadata_path}" _release_json)
  string(JSON _asset_count ERROR_VARIABLE _json_error LENGTH "${_release_json}" assets)
  if(NOT _json_error STREQUAL "NOTFOUND" OR _asset_count EQUAL 0)
    return()
  endif()

  math(EXPR _last_asset_index "${_asset_count} - 1")
  foreach(_asset_index RANGE 0 ${_last_asset_index})
    string(JSON _name ERROR_VARIABLE _name_error GET "${_release_json}" assets ${_asset_index} name)
    if(NOT _name_error STREQUAL "NOTFOUND" OR NOT _name STREQUAL asset_name)
      continue()
    endif()

    string(JSON _digest ERROR_VARIABLE _digest_error GET "${_release_json}" assets ${_asset_index} digest)
    if(_digest_error STREQUAL "NOTFOUND" AND _digest MATCHES "^sha256:([0-9A-Fa-f]+)$")
      set(_sha256 "${CMAKE_MATCH_1}")
      string(LENGTH "${_sha256}" _sha256_length)
      if(_sha256_length EQUAL 64)
        string(TOLOWER "${_sha256}" _sha256)
        set(${output_variable} "${_sha256}" PARENT_SCOPE)
      endif()
    endif()
    return()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# ZakoVirtualMouse (public release assets)
# ---------------------------------------------------------------------------
function(_fetch_vmouse_impl _files output_verified)
  set(${output_verified} FALSE PARENT_SCOPE)
  message(STATUS "Fetching ZakoVirtualMouse ${VMOUSE_DRIVER_VERSION} ...")

  file(MAKE_DIRECTORY "${VMOUSE_DRIVER_DIR}")

  if(NOT VMOUSE_PUBLIC_REPO)
    message(WARNING "  VMOUSE_PUBLIC_REPO is empty")
    return()
  endif()

  set(_metadata_path "${VMOUSE_DRIVER_DIR}/.release-metadata.json")
  _download_github_release_metadata(
    "${VMOUSE_PUBLIC_REPO}"
    "${VMOUSE_DRIVER_VERSION}"
    "${_metadata_path}")
  if(NOT EXISTS "${_metadata_path}")
    message(WARNING "  GitHub release metadata is unavailable for ${VMOUSE_DRIVER_VERSION}")
    return()
  endif()

  message(STATUS "  Downloading from public repo ${VMOUSE_PUBLIC_REPO} ...")
  foreach(_f ${_files})
    _github_release_asset_sha256("${_metadata_path}" "${_f}" _expected_sha256)
    if(NOT _expected_sha256)
      message(WARNING "  GitHub release metadata has no SHA-256 digest for ${_f}")
      file(REMOVE "${VMOUSE_DRIVER_DIR}/${_f}")
      continue()
    endif()
    set(_url "https://github.com/${VMOUSE_PUBLIC_REPO}/releases/download/${VMOUSE_DRIVER_VERSION}/${_f}")
    _driver_download("${_url}" "${VMOUSE_DRIVER_DIR}/${_f}" "${_expected_sha256}")
  endforeach()

  set(_all_verified TRUE)
  foreach(_f ${_files})
    if(NOT EXISTS "${VMOUSE_DRIVER_DIR}/${_f}")
      set(_all_verified FALSE)
      break()
    endif()
  endforeach()
  set(${output_verified} ${_all_verified} PARENT_SCOPE)
endfunction()

# The vmouse assets are downloaded as bare filenames with no version in them, so
# a cache populated by an older VMOUSE_DRIVER_VERSION looks complete forever and
# a pin bump silently ships the old driver on any tree that was configured
# before. Stamp the tag next to the files and wipe the cache when it changes.
function(_fetch_vmouse)
  set(_files ZakoVirtualMouse.dll ZakoVirtualMouse.inf ZakoVirtualMouse.cat ZakoVirtualMouse.cer)
  set(_marker "${VMOUSE_DRIVER_DIR}/.release-version")
  set(_expected_marker "${VMOUSE_PUBLIC_REPO}|${VMOUSE_DRIVER_VERSION}")

  set(_stamp_ok FALSE)
  if(EXISTS "${_marker}")
    file(READ "${_marker}" _current)
    string(STRIP "${_current}" _current)
    if("${_current}" STREQUAL "${_expected_marker}")
      set(_stamp_ok TRUE)
    endif()
  endif()

  if(NOT _stamp_ok AND EXISTS "${VMOUSE_DRIVER_DIR}")
    message(STATUS "  vmouse cache source or version changed; clearing ${VMOUSE_DRIVER_DIR}")
    file(REMOVE_RECURSE "${VMOUSE_DRIVER_DIR}")
  endif()

  _fetch_vmouse_impl("${_files}" _all_verified)

  # Only stamp a complete, digest-verified set. A partial download must retry
  # on the next configure instead of being treated as a good cache.
  if(_all_verified)
    file(WRITE "${_marker}" "${_expected_marker}\n")
  else()
    file(REMOVE "${_marker}")
  endif()
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
  set(_marker "${NEFCON_DRIVER_DIR}/.release-version")
  set(_expected_marker "${NEFCON_VERSION}:${_NEFCON_EFFECTIVE_SHA256}")

  set(_stamp_ok FALSE)
  if(EXISTS "${_marker}")
    file(READ "${_marker}" _current)
    string(STRIP "${_current}" _current)
    if("${_current}" STREQUAL "${_expected_marker}")
      set(_stamp_ok TRUE)
    endif()
  endif()

  if(NOT _stamp_ok AND EXISTS "${NEFCON_DRIVER_DIR}")
    message(STATUS "  nefcon cache is not ${NEFCON_VERSION}; clearing ${NEFCON_DRIVER_DIR}")
    file(REMOVE_RECURSE "${NEFCON_DRIVER_DIR}")
  endif()

  _driver_download("${_zip_url}" "${_zip}" "${_NEFCON_EFFECTIVE_SHA256}")

  if(EXISTS "${_zip}" AND NOT EXISTS "${NEFCON_DRIVER_DIR}/nefconw.exe")
    set(_tmp "${DRIVER_DEPS_CACHE}/_nefcon_extract")
    file(REMOVE_RECURSE "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_tmp}")
    file(MAKE_DIRECTORY "${NEFCON_DRIVER_DIR}")
    file(COPY_FILE "${_tmp}/x64/nefconw.exe" "${NEFCON_DRIVER_DIR}/nefconw.exe")
    file(WRITE "${_marker}" "${_expected_marker}\n")
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
