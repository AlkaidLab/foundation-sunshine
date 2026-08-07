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
set(_gui_exe_cached FALSE)

if(NOT FETCH_GUI)
  message(STATUS "GUI download disabled (FETCH_GUI=OFF)")
  return()
endif()

# Skip if already downloaded
if(EXISTS "${GUI_DIR}/sunshine-gui.exe")
  set(_gui_exe_cached TRUE)
  if(EXISTS "${GUI_DIR}/WebView2Loader.dll")
    message(STATUS "GUI binary already cached at ${GUI_DIR}")
    return()
  endif()
  message(STATUS "GUI binary already cached at ${GUI_DIR}; checking for WebView2Loader.dll")
endif()

file(MAKE_DIRECTORY "${GUI_DIR}")

find_program(_CURL curl REQUIRED)

# GitHub token (optional, for rate limits)
if(NOT GITHUB_TOKEN AND DEFINED ENV{GITHUB_TOKEN})
  set(GITHUB_TOKEN "$ENV{GITHUB_TOKEN}")
endif()

# How many releases to look back through when GUI_VERSION is "latest".
set(_gui_release_scan_count 20)

# Resolve release URL.
#
# "latest" deliberately does not use /releases/latest. The GUI repo publishes
# the release before its build uploads the binaries, so a failed or still
# running build leaves an asset-less release sitting at /releases/latest. Since
# a missing sunshine-gui.exe is a FATAL_ERROR for the GUI tray build, trusting
# that endpoint blind means one upstream hiccup blocks every Windows package.
# Ask for the release list instead and take the newest entry that actually
# carries the binary.
if(GUI_VERSION STREQUAL "latest")
  set(_api_url "https://api.github.com/repos/${GUI_REPO}/releases?per_page=${_gui_release_scan_count}")
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
file(REMOVE "${_json}")

if(GUI_VERSION STREQUAL "latest")
  # Narrow the release list down to a single release object, so everything
  # downstream keeps working against one release exactly as before.
  string(JSON _release_count ERROR_VARIABLE _json_error LENGTH "${_json_content}")
  if(_json_error)
    message(WARNING "Could not parse the GUI release list: ${_json_error}")
    message(WARNING "GUI will not be available. Build it manually or set FETCH_GUI=OFF.")
    return()
  endif()

  set(_selected_release "")
  set(_selected_tag "")

  if(_release_count GREATER 0)
    math(EXPR _last_release_index "${_release_count} - 1")
    foreach(_release_index RANGE 0 ${_last_release_index})
      string(JSON _release GET "${_json_content}" ${_release_index})
      string(JSON _release_tag GET "${_release}" tag_name)

      # /releases/latest skips drafts and prereleases; match that so switching
      # endpoints does not quietly start shipping prerelease GUIs.
      string(JSON _is_draft GET "${_release}" draft)
      string(JSON _is_prerelease GET "${_release}" prerelease)
      if(_is_draft OR _is_prerelease)
        continue()
      endif()

      string(JSON _asset_count ERROR_VARIABLE _assets_error LENGTH "${_release}" assets)
      if(_assets_error)
        set(_asset_count 0)
      endif()

      set(_release_has_gui FALSE)
      if(_asset_count GREATER 0)
        math(EXPR _last_asset_index "${_asset_count} - 1")
        foreach(_asset_index RANGE 0 ${_last_asset_index})
          string(JSON _asset_name GET "${_release}" assets ${_asset_index} name)
          if(_asset_name STREQUAL "sunshine-gui.exe")
            set(_release_has_gui TRUE)
            break()
          endif()
        endforeach()
      endif()

      if(_release_has_gui)
        set(_selected_release "${_release}")
        set(_selected_tag "${_release_tag}")
        break()
      endif()

      message(STATUS "  Skipping GUI release ${_release_tag}: no sunshine-gui.exe asset")
    endforeach()
  endif()

  if(NOT _selected_release)
    message(WARNING
      "None of the newest ${_release_count} releases in ${GUI_REPO} carry sunshine-gui.exe")
    message(WARNING "GUI will not be available. Build it manually or set FETCH_GUI=OFF.")
    return()
  endif()

  message(STATUS "  Using GUI release ${_selected_tag}")
  set(_json_content "${_selected_release}")
endif()

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
if(NOT _gui_exe_cached)
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
