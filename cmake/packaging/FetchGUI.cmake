# FetchGUI.cmake — Download pre-built Sunshine GUI from GitHub Releases
#
# Downloads the Tauri-based GUI binary at configure time instead of building
# it from source. This removes the Rust/Cargo/Node.js dependency from the
# main build.
#
# Configuration (CMake cache variables):
#   FETCH_GUI             — Enable/disable GUI download (default: ON)
#   GUI_VERSION           — Release tag to download (e.g. v0.2.9)
#   GUI_REPO              — GitHub repo (default: qiin2333/sunshine-control-panel)
#
# Output variables (CACHE FORCE):
#   GUI_DIR               — Directory containing sunshine-gui.exe

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

option(FETCH_GUI "Download pre-built GUI from GitHub Releases" ON)

set(GUI_VERSION "latest" CACHE STRING "Sunshine GUI release tag (or 'latest')")
set(GUI_REPO "qiin2333/sunshine-control-panel" CACHE STRING "GUI GitHub repository")

set(GUI_DIR "${CMAKE_BINARY_DIR}/_gui" CACHE PATH "GUI binary directory" FORCE)

if(NOT FETCH_GUI)
  message(STATUS "GUI download disabled (FETCH_GUI=OFF)")
  return()
endif()

# Skip if already downloaded
if(EXISTS "${GUI_DIR}/sunshine-gui.exe")
  message(STATUS "GUI binary already cached at ${GUI_DIR}")
  return()
endif()

file(MAKE_DIRECTORY "${GUI_DIR}")

find_program(_CURL curl REQUIRED)

# GitHub token (optional, for rate limits)
if(NOT GITHUB_TOKEN AND DEFINED ENV{GITHUB_TOKEN})
  set(GITHUB_TOKEN "$ENV{GITHUB_TOKEN}")
endif()

# Resolve release URL
if(GUI_VERSION STREQUAL "latest")
  set(_api_url "https://api.github.com/repos/${GUI_REPO}/releases/latest")
else()
  set(_api_url "https://api.github.com/repos/${GUI_REPO}/releases/tags/${GUI_VERSION}")
endif()

message(STATUS "Fetching Sunshine GUI ${GUI_VERSION} from ${GUI_REPO} ...")

# Build auth header args
set(_auth_args)
if(GITHUB_TOKEN)
  set(_auth_args -H "Authorization: token ${GITHUB_TOKEN}")
endif()

# Query release to get sunshine-gui.exe asset URL
set(_json "${CMAKE_BINARY_DIR}/_gui_release.json")
execute_process(
  COMMAND "${_CURL}" -fsSL
    ${_auth_args}
    -H "Accept: application/vnd.github+json"
    -o "${_json}"
    "${_api_url}"
  RESULT_VARIABLE _rc
  ERROR_VARIABLE _err)

if(NOT _rc EQUAL 0)
  message(WARNING "Failed to query GUI release API (${_rc}): ${_err}")
  message(WARNING "GUI will not be available. Build it manually or set FETCH_GUI=OFF.")
  return()
endif()

# Parse asset download URLs from JSON
file(READ "${_json}" _json_content)

# Extract sunshine-gui.exe browser_download_url
string(REGEX MATCH "\"browser_download_url\"[^\"]*\"(https://[^\"]*sunshine-gui\\.exe)\"" _m "${_json_content}")
if(_m)
  set(_gui_url "${CMAKE_MATCH_1}")
else()
  # Try API asset URL for private repos
  string(REGEX MATCH "\"url\":[ ]*\"(https://api\\.github\\.com/repos/[^\"]+/assets/[0-9]+)\"[^}]*\"name\":[ ]*\"sunshine-gui\\.exe\"" _m2 "${_json_content}")
  if(_m2)
    set(_gui_api_url "${CMAKE_MATCH_1}")
  else()
    message(WARNING "Could not find sunshine-gui.exe in release assets")
    file(REMOVE "${_json}")
    return()
  endif()
endif()

# Download sunshine-gui.exe
if(_gui_url)
  message(STATUS "  Downloading sunshine-gui.exe ...")
  execute_process(
    COMMAND "${_CURL}" -fsSL
      ${_auth_args}
      -o "${GUI_DIR}/sunshine-gui.exe"
      -L "${_gui_url}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
elseif(_gui_api_url)
  message(STATUS "  Downloading sunshine-gui.exe via API ...")
  execute_process(
    COMMAND "${_CURL}" -fsSL
      ${_auth_args}
      -H "Accept: application/octet-stream"
      -o "${GUI_DIR}/sunshine-gui.exe"
      "${_gui_api_url}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
endif()

if(NOT _rc EQUAL 0)
  message(WARNING "  Download failed (${_rc}): ${_err}")
  file(REMOVE "${GUI_DIR}/sunshine-gui.exe")
endif()

# Try downloading WebView2Loader.dll (optional, Tauri 2 may embed it)
string(REGEX MATCH "\"browser_download_url\"[^\"]*\"(https://[^\"]*WebView2Loader\\.dll)\"" _wv "${_json_content}")
if(_wv)
  set(_wv_url "${CMAKE_MATCH_1}")
  message(STATUS "  Downloading WebView2Loader.dll ...")
  execute_process(
    COMMAND "${_CURL}" -fsSL
      ${_auth_args}
      -o "${GUI_DIR}/WebView2Loader.dll"
      -L "${_wv_url}"
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    file(REMOVE "${GUI_DIR}/WebView2Loader.dll")
  endif()
endif()

file(REMOVE "${_json}")

# Verify
if(NOT EXISTS "${GUI_DIR}/sunshine-gui.exe")
  message(WARNING "GUI download failed. sunshine-gui.exe will not be available in the install.")
else()
  file(SIZE "${GUI_DIR}/sunshine-gui.exe" _size)
  math(EXPR _size_mb "${_size} / 1048576")
  message(STATUS "  GUI downloaded successfully (${_size_mb} MB)")
endif()
