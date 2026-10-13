# Build scripts generate this before configuration. Bind exact managed bytes.
if(NOT WIN32)
  return()
endif()
if(NOT DEFINED HIDMAESTRO_CORE_PATCH_DIR)
  set(HIDMAESTRO_CORE_PATCH_DIR "${CMAKE_BINARY_DIR}/hidmaestro-core-patch" CACHE PATH
      "Verified Foundation HIDMaestro Core patch build output")
endif()
set(_hm_core "${HIDMAESTRO_CORE_PATCH_DIR}/HIDMaestro.Core.dll")
set(_hm_metadata "${HIDMAESTRO_CORE_PATCH_DIR}/core-patch.json")
if(EXISTS "${_hm_core}" AND EXISTS "${_hm_metadata}")
  file(READ "${_hm_metadata}" _hm_json)
  string(JSON _hm_schema GET "${_hm_json}" schema)
  string(JSON _hm_expected GET "${_hm_json}" sha256)
  string(JSON _hm_original GET "${_hm_json}" original_sha256)
  string(JSON _hm_version GET "${_hm_json}" upstream_version)
  string(JSON _hm_patch GET "${_hm_json}" patch)
  file(SHA256 "${_hm_core}" _hm_actual)
  if(NOT _hm_schema EQUAL 1 OR NOT _hm_actual STREQUAL _hm_expected OR NOT _hm_version STREQUAL "v1.7.3"
      OR NOT _hm_patch STREQUAL "registry-isolation-1"
      OR NOT _hm_original STREQUAL "95c6d3ec0d41cd7b3adda80da3ffbcd19c9466cc50e08a973c7745a16c7e0bde")
    message(FATAL_ERROR "HIDMaestro Core patch metadata or digest does not match its build")
  endif()
  target_compile_definitions(sunshine PRIVATE
      SUNSHINE_DS5_CORE_PATCH_SHA256="${_hm_actual}"
      SUNSHINE_DS5_ORIGINAL_CORE_SHA256="${_hm_original}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hm_core}" "${_hm_metadata}")
  set(SUNSHINE_HIDMAESTRO_CORE_PATCH_AVAILABLE ON)
else()
  message(STATUS "HIDMaestro Core patch not built; DualSense runtime will be unavailable")
  set(SUNSHINE_HIDMAESTRO_CORE_PATCH_AVAILABLE OFF)
endif()
