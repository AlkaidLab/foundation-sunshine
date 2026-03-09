/**
 * @file src/platform/windows/capture_plugin/capture_plugin_loader.h
 * @brief Capture plugin loader - discovers and loads plugin DLLs.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "capture_plugin_api.h"

namespace platf::capture_plugin {

  /**
   * @brief Represents a loaded capture plugin DLL.
   */
  struct loaded_plugin_t {
    std::string name;           ///< Plugin name from get_info()
    std::string version;        ///< Plugin version string
    std::string dll_path;       ///< Full path to the DLL
    uint32_t supported_mem_types; ///< Bitmask of supported memory types

    sunshine_capture_plugin_vtable_t vtable; ///< Function table

    void *dll_handle;           ///< Platform-specific DLL handle (HMODULE on Windows)
  };

  /**
   * @brief Load a single plugin DLL.
   * @param dll_path Path to the DLL file.
   * @return Loaded plugin info, or nullptr on failure.
   */
  std::unique_ptr<loaded_plugin_t>
  load_plugin(const std::filesystem::path &dll_path);

  /**
   * @brief Unload a plugin and free the DLL.
   */
  void
  unload_plugin(loaded_plugin_t *plugin);

  /**
   * @brief Discover and load all plugins from the plugins directory.
   * @return List of successfully loaded plugins.
   */
  std::vector<std::unique_ptr<loaded_plugin_t>>
  discover_plugins();

  /**
   * @brief Find a loaded plugin by name (case-insensitive).
   * @param plugins The list of loaded plugins.
   * @param name Plugin name to search for.
   * @return Pointer to the plugin, or nullptr if not found.
   */
  loaded_plugin_t *
  find_plugin(const std::vector<std::unique_ptr<loaded_plugin_t>> &plugins, const std::string &name);

}  // namespace platf::capture_plugin
