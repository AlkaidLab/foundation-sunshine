# FetchGUI.cmake — Download the pre-built Sunshine GUI bundle from GitHub Releases
#
# The Control Panel release publishes one ZIP containing the GUI executable and
# every built-in native plugin. Fetching one archive keeps those files on the
# same release boundary and prevents mixed-version installations.
#
# Configuration (CMake cache variables):
#   FETCH_GUI             — Enable/disable GUI download (default: ON)
#   GUI_VERSION           — Release tag to download (e.g. v0.4.38)
#   GUI_REPO              — GitHub repo (default: qiin2333/sunshine-control-panel)
#   GUI_ASSET_NAME        — Release ZIP asset name
#
# Output variables (CACHE FORCE):
#   GUI_DIR               — Directory containing the extracted GUI bundle

include_guard(GLOBAL)

if(NOT WIN32)
  return()
endif()

option(FETCH_GUI "Download pre-built GUI from GitHub Releases" ON)

set(GUI_VERSION "latest" CACHE STRING "Sunshine GUI release tag (or 'latest')")
set(GUI_REPO "qiin2333/sunshine-control-panel" CACHE STRING "GUI GitHub repository")
set(GUI_ASSET_NAME "sunshine-gui-windows-x64.zip" CACHE STRING "Sunshine GUI bundle asset name")
set(GUI_DIR "${CMAKE_BINARY_DIR}/_gui" CACHE PATH "GUI binary directory" FORCE)

if(NOT FETCH_GUI)
  message(STATUS "GUI download disabled (FETCH_GUI=OFF)")
  return()
endif()

set(_gui_stamp "${GUI_DIR}/.sunshine-gui-bundle")
set(_cached_gui_repo "")
set(_cached_gui_asset "")
set(_cached_gui_request "")
if(EXISTS "${_gui_stamp}")
  file(STRINGS "${_gui_stamp}" _cached_gui_stamp)
  list(LENGTH _cached_gui_stamp _cached_gui_stamp_size)
  if(_cached_gui_stamp_size GREATER_EQUAL 4)
    list(GET _cached_gui_stamp 0 _cached_gui_repo)
    list(GET _cached_gui_stamp 1 _cached_gui_asset)
    list(GET _cached_gui_stamp 2 _cached_gui_request)
  endif()
endif()
if(EXISTS "${GUI_DIR}/sunshine-gui.exe" AND
   EXISTS "${GUI_DIR}/alkaidlab-plugin-stylus.dll" AND
   EXISTS "${_gui_stamp}" AND
   "${_cached_gui_repo}" STREQUAL "${GUI_REPO}" AND
   "${_cached_gui_asset}" STREQUAL "${GUI_ASSET_NAME}" AND
   "${_cached_gui_request}" STREQUAL "${GUI_VERSION}")
  file(SIZE "${GUI_DIR}/sunshine-gui.exe" _cached_gui_size)
  file(SIZE "${GUI_DIR}/alkaidlab-plugin-stylus.dll" _cached_plugin_size)
  if(_cached_gui_size GREATER 0 AND _cached_plugin_size GREATER 0)
    message(STATUS "GUI bundle already cached at ${GUI_DIR}")
    return()
  endif()
endif()

file(MAKE_DIRECTORY "${GUI_DIR}")
find_program(_CURL curl REQUIRED)

if(NOT GITHUB_TOKEN AND DEFINED ENV{GITHUB_TOKEN})
  set(GITHUB_TOKEN "$ENV{GITHUB_TOKEN}")
endif()

set(_auth_args)
if(GITHUB_TOKEN)
  set(_auth_args -H "Authorization: token ${GITHUB_TOKEN}")
endif()

set(_gui_release_scan_count 20)
if(GUI_VERSION STREQUAL "latest")
  set(_api_url "https://api.github.com/repos/${GUI_REPO}/releases?per_page=${_gui_release_scan_count}")
else()
  set(_api_url "https://api.github.com/repos/${GUI_REPO}/releases/tags/${GUI_VERSION}")
endif()

message(STATUS "Fetching Sunshine GUI ${GUI_VERSION} from ${GUI_REPO} ...")
set(_release_json "${CMAKE_BINARY_DIR}/_gui_release.json")
execute_process(
  COMMAND "${_CURL}" -fsSL
    ${_auth_args}
    -H "Accept: application/vnd.github+json"
    -o "${_release_json}"
    "${_api_url}"
  RESULT_VARIABLE _query_result
  ERROR_VARIABLE _query_error)

if(NOT _query_result EQUAL 0)
  message(WARNING "Failed to query GUI release API (${_query_result}): ${_query_error}")
  message(WARNING "GUI will not be available. Build it manually or set FETCH_GUI=OFF.")
  file(REMOVE "${_release_json}")
  return()
endif()

file(READ "${_release_json}" _release_json_content)
file(REMOVE "${_release_json}")

set(_selected_release "")
set(_selected_tag "")
if(GUI_VERSION STREQUAL "latest")
  string(JSON _release_count ERROR_VARIABLE _release_error LENGTH "${_release_json_content}")
  if(_release_error)
    message(WARNING "Could not parse the GUI release list: ${_release_error}")
    return()
  endif()

  if(_release_count GREATER 0)
    math(EXPR _last_release_index "${_release_count} - 1")
    foreach(_release_index RANGE 0 ${_last_release_index})
      string(JSON _release GET "${_release_json_content}" ${_release_index})
      string(JSON _release_tag GET "${_release}" tag_name)
      string(JSON _is_draft GET "${_release}" draft)
      string(JSON _is_prerelease GET "${_release}" prerelease)
      if(_is_draft OR _is_prerelease)
        continue()
      endif()

      string(JSON _asset_count ERROR_VARIABLE _assets_error LENGTH "${_release}" assets)
      if(_assets_error)
        set(_asset_count 0)
      endif()
      set(_release_has_bundle FALSE)
      if(_asset_count GREATER 0)
        math(EXPR _last_asset_index "${_asset_count} - 1")
        foreach(_asset_index RANGE 0 ${_last_asset_index})
          string(JSON _asset_name GET "${_release}" assets ${_asset_index} name)
          if(_asset_name STREQUAL "${GUI_ASSET_NAME}")
            set(_release_has_bundle TRUE)
            break()
          endif()
        endforeach()
      endif()

      if(_release_has_bundle)
        set(_selected_release "${_release}")
        set(_selected_tag "${_release_tag}")
        break()
      endif()
      message(STATUS "  Skipping GUI release ${_release_tag}: no ${GUI_ASSET_NAME} asset")
    endforeach()
  endif()
else()
  set(_selected_release "${_release_json_content}")
  set(_selected_tag "${GUI_VERSION}")
endif()

if(NOT _selected_release)
  message(WARNING "No suitable GUI release contains ${GUI_ASSET_NAME}")
  message(WARNING "GUI will not be available. Build it manually or set FETCH_GUI=OFF.")
  return()
endif()

set(_bundle_download_url "")
set(_bundle_api_url "")
string(JSON _asset_count ERROR_VARIABLE _assets_error LENGTH "${_selected_release}" assets)
if(NOT _assets_error AND _asset_count GREATER 0)
  math(EXPR _last_asset_index "${_asset_count} - 1")
  foreach(_asset_index RANGE 0 ${_last_asset_index})
    string(JSON _asset_name GET "${_selected_release}" assets ${_asset_index} name)
    if(NOT _asset_name STREQUAL "${GUI_ASSET_NAME}")
      continue()
    endif()
    string(JSON _bundle_download_url GET "${_selected_release}" assets ${_asset_index} browser_download_url)
    string(JSON _bundle_api_url GET "${_selected_release}" assets ${_asset_index} url)
    break()
  endforeach()
endif()

if(NOT _bundle_download_url AND NOT _bundle_api_url)
  message(WARNING "Could not resolve ${GUI_ASSET_NAME} from GUI release ${_selected_tag}")
  return()
endif()

set(_bundle_file "${CMAKE_BINARY_DIR}/_gui_bundle.zip")
file(REMOVE "${_bundle_file}")
if(_bundle_download_url)
  execute_process(
    COMMAND "${_CURL}" -fsSL
      ${_auth_args}
      -o "${_bundle_file}"
      -L "${_bundle_download_url}"
    RESULT_VARIABLE _download_result
    ERROR_VARIABLE _download_error)
else()
  execute_process(
    COMMAND "${_CURL}" -fsSL
      ${_auth_args}
      -H "Accept: application/octet-stream"
      -o "${_bundle_file}"
      "${_bundle_api_url}"
    RESULT_VARIABLE _download_result
    ERROR_VARIABLE _download_error)
endif()

if(NOT _download_result EQUAL 0)
  message(WARNING "GUI bundle download failed (${_download_result}): ${_download_error}")
  file(REMOVE "${_bundle_file}")
  return()
endif()

set(_extract_dir "${CMAKE_BINARY_DIR}/_gui_extract")
file(REMOVE_RECURSE "${_extract_dir}")
file(MAKE_DIRECTORY "${_extract_dir}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xvf "${_bundle_file}"
  WORKING_DIRECTORY "${_extract_dir}"
  RESULT_VARIABLE _extract_result
  ERROR_VARIABLE _extract_error)
file(REMOVE "${_bundle_file}")

if(NOT _extract_result EQUAL 0)
  message(WARNING "GUI bundle extraction failed (${_extract_result}): ${_extract_error}")
  file(REMOVE_RECURSE "${_extract_dir}")
  return()
endif()

if(NOT EXISTS "${_extract_dir}/sunshine-gui.exe" OR
   NOT EXISTS "${_extract_dir}/alkaidlab-plugin-stylus.dll")
  message(WARNING "GUI bundle ${GUI_ASSET_NAME} is incomplete")
  file(REMOVE_RECURSE "${_extract_dir}")
  return()
endif()
file(SIZE "${_extract_dir}/sunshine-gui.exe" _extracted_gui_size)
file(SIZE "${_extract_dir}/alkaidlab-plugin-stylus.dll" _extracted_plugin_size)
if(_extracted_gui_size EQUAL 0 OR _extracted_plugin_size EQUAL 0)
  message(WARNING "GUI bundle ${GUI_ASSET_NAME} contains an empty required file")
  file(REMOVE_RECURSE "${_extract_dir}")
  return()
endif()

# Publish only after validating every required file. Invalidate the old stamp
# first so an interrupted copy cannot make a mixed bundle look complete.
file(REMOVE "${_gui_stamp}")
configure_file("${_extract_dir}/sunshine-gui.exe" "${GUI_DIR}/sunshine-gui.exe" COPYONLY)
configure_file(
  "${_extract_dir}/alkaidlab-plugin-stylus.dll"
  "${GUI_DIR}/alkaidlab-plugin-stylus.dll"
  COPYONLY)
if(EXISTS "${_extract_dir}/WebView2Loader.dll")
  configure_file(
    "${_extract_dir}/WebView2Loader.dll"
    "${GUI_DIR}/WebView2Loader.dll"
    COPYONLY)
else()
  file(REMOVE "${GUI_DIR}/WebView2Loader.dll")
endif()
file(WRITE "${_gui_stamp}"
  "${GUI_REPO}\n${GUI_ASSET_NAME}\n${GUI_VERSION}\n${_selected_tag}\n")
file(REMOVE_RECURSE "${_extract_dir}")

math(EXPR _bundle_size_mb "(${_extracted_gui_size} + ${_extracted_plugin_size}) / 1048576")
message(STATUS "  GUI bundle ${_selected_tag} extracted successfully (${_bundle_size_mb} MB)")
