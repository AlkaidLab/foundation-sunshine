/**
 * @file src/platform/windows/capture_plugin/capture_plugin_loader.cpp
 * @brief Implementation of capture plugin loader.
 */
#include "capture_plugin_loader.h"

#include <algorithm>
#include <cctype>

#include <Windows.h>

#include "src/logging.h"

namespace platf::capture_plugin {

  namespace {

    /**
     * @brief Resolve a single function from the DLL.
     * @return Function pointer, or nullptr if not found.
     */
    template <typename Fn>
    Fn
    resolve_fn(HMODULE dll, const char *name) {
      auto fn = reinterpret_cast<Fn>(GetProcAddress(dll, name));
      if (!fn) {
        BOOST_LOG(warning) << "Plugin missing export: " << name;
      }
      return fn;
    }

    /**
     * @brief Get the plugins directory path.
     */
    std::filesystem::path
    get_plugins_dir() {
      wchar_t module_path[MAX_PATH];
      GetModuleFileNameW(nullptr, module_path, MAX_PATH);
      return std::filesystem::path(module_path).parent_path() / "plugins";
    }

  }  // namespace

  std::unique_ptr<loaded_plugin_t>
  load_plugin(const std::filesystem::path &dll_path) {
    BOOST_LOG(info) << "Loading capture plugin: " << dll_path.string();

    auto dll = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) {
      BOOST_LOG(error) << "Failed to load plugin DLL: " << dll_path.string()
                       << " (error " << GetLastError() << ")";
      return nullptr;
    }

    auto plugin = std::make_unique<loaded_plugin_t>();
    plugin->dll_handle = dll;
    plugin->dll_path = dll_path.string();

    // Resolve required function: get_info
    plugin->vtable.get_info = resolve_fn<sunshine_capture_get_info_fn>(dll, "sunshine_capture_get_info");
    if (!plugin->vtable.get_info) {
      BOOST_LOG(error) << "Plugin missing required export: sunshine_capture_get_info";
      FreeLibrary(dll);
      return nullptr;
    }

    // Get plugin info and validate ABI version
    sunshine_capture_plugin_info_t info {};
    if (plugin->vtable.get_info(&info) != 0) {
      BOOST_LOG(error) << "Plugin sunshine_capture_get_info() failed";
      FreeLibrary(dll);
      return nullptr;
    }

    if (info.abi_version != SUNSHINE_CAPTURE_PLUGIN_ABI_VERSION) {
      BOOST_LOG(error) << "Plugin ABI version mismatch: expected "
                       << SUNSHINE_CAPTURE_PLUGIN_ABI_VERSION
                       << ", got " << info.abi_version;
      FreeLibrary(dll);
      return nullptr;
    }

    plugin->name = info.name ? info.name : "unknown";
    plugin->version = info.version ? info.version : "0.0.0";
    plugin->supported_mem_types = info.supported_mem_types;

    // Resolve remaining functions
    plugin->vtable.enum_displays = resolve_fn<sunshine_capture_enum_displays_fn>(dll, "sunshine_capture_enum_displays");
    plugin->vtable.create_session = resolve_fn<sunshine_capture_create_session_fn>(dll, "sunshine_capture_create_session");
    plugin->vtable.destroy_session = resolve_fn<sunshine_capture_destroy_session_fn>(dll, "sunshine_capture_destroy_session");
    plugin->vtable.next_frame = resolve_fn<sunshine_capture_next_frame_fn>(dll, "sunshine_capture_next_frame");
    plugin->vtable.release_frame = resolve_fn<sunshine_capture_release_frame_fn>(dll, "sunshine_capture_release_frame");
    plugin->vtable.is_hdr = resolve_fn<sunshine_capture_is_hdr_fn>(dll, "sunshine_capture_is_hdr");
    plugin->vtable.interrupt = resolve_fn<sunshine_capture_interrupt_fn>(dll, "sunshine_capture_interrupt");

    // Validate required functions
    if (!plugin->vtable.create_session || !plugin->vtable.destroy_session || !plugin->vtable.next_frame) {
      BOOST_LOG(error) << "Plugin missing required exports (create_session, destroy_session, or next_frame)";
      FreeLibrary(dll);
      return nullptr;
    }

    BOOST_LOG(info) << "Loaded capture plugin: " << plugin->name << " v" << plugin->version;
    return plugin;
  }

  void
  unload_plugin(loaded_plugin_t *plugin) {
    if (plugin && plugin->dll_handle) {
      BOOST_LOG(info) << "Unloading capture plugin: " << plugin->name;
      FreeLibrary(static_cast<HMODULE>(plugin->dll_handle));
      plugin->dll_handle = nullptr;
    }
  }

  std::vector<std::unique_ptr<loaded_plugin_t>>
  discover_plugins() {
    std::vector<std::unique_ptr<loaded_plugin_t>> plugins;

    auto plugins_dir = get_plugins_dir();
    if (!std::filesystem::exists(plugins_dir)) {
      BOOST_LOG(debug) << "No plugins directory found at: " << plugins_dir.string();
      return plugins;
    }

    BOOST_LOG(info) << "Scanning for capture plugins in: " << plugins_dir.string();

    for (const auto &entry : std::filesystem::directory_iterator(plugins_dir)) {
      if (!entry.is_regular_file()) continue;

      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext != ".dll") continue;

      auto plugin = load_plugin(entry.path());
      if (plugin) {
        plugins.push_back(std::move(plugin));
      }
    }

    BOOST_LOG(info) << "Discovered " << plugins.size() << " capture plugin(s)";
    return plugins;
  }

  loaded_plugin_t *
  find_plugin(const std::vector<std::unique_ptr<loaded_plugin_t>> &plugins, const std::string &name) {
    for (const auto &plugin : plugins) {
      // Case-insensitive comparison
      auto plugin_name = plugin->name;
      auto search_name = name;
      std::transform(plugin_name.begin(), plugin_name.end(), plugin_name.begin(), ::tolower);
      std::transform(search_name.begin(), search_name.end(), search_name.begin(), ::tolower);
      if (plugin_name == search_name) {
        return plugin.get();
      }
    }
    return nullptr;
  }

}  // namespace platf::capture_plugin
