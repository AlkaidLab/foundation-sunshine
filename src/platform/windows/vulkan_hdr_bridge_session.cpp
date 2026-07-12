#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vulkan_hdr_bridge_session.h"

#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "src/logging.h"
#include "src/platform/run_command.h"

namespace platf::vulkan_hdr_bridge {
  namespace {
    constexpr wchar_t kImplicitLayersKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
    constexpr wchar_t kManifestName[] = L"VkLayer_zako_virtual_hdr.json";
    constexpr wchar_t kLayerDllName[] = L"zako_vulkan_hdr_bridge.dll";
    constexpr wchar_t kProbeName[] = L"vulkan_hdr_probe.exe";
    constexpr wchar_t kCacheName[] = L"zako_vulkan_hdr_capabilities.bin";

    std::mutex state_mutex;
    bool registered = false;
    std::string current_state = "idle";
    std::string current_message = "Waiting for an HDR VDD stream.";

    void
    set_status(std::string state, std::string message) {
      current_state = std::move(state);
      current_message = std::move(message);
    }

    std::filesystem::path
    executable_directory() {
      std::wstring buffer(32768, L'\0');
      const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length == 0 || length >= buffer.size()) {
        return {};
      }
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path();
    }

    std::filesystem::path
    artifact_directory() {
      return executable_directory() / L"tools" / L"vulkan_hdr";
    }

    bool
    artifacts_installed() {
      const auto directory = artifact_directory();
      return std::filesystem::exists(directory / kManifestName) &&
             std::filesystem::exists(directory / kLayerDllName) &&
             std::filesystem::exists(directory / kProbeName);
    }

    std::wstring
    lowercase(std::wstring value) {
      std::transform(value.begin(), value.end(), value.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
      });
      return value;
    }

    bool
    is_bridge_manifest_value(const std::wstring &value_name) {
      return lowercase(std::filesystem::path(value_name).filename().wstring()) == lowercase(kManifestName);
    }

    bool
    remove_machine_bridge_values(bool log_errors = true) {
      HKEY key = nullptr;
      const LONG open_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kImplicitLayersKey, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
      if (open_status == ERROR_FILE_NOT_FOUND) {
        return true;
      }
      // Current releases use HKCU through the interactive-user helper. A
      // non-elevated portable Sunshine cannot remove legacy HKLM values, and
      // that must not prevent the per-user path from working.
      if (open_status == ERROR_ACCESS_DENIED) {
        return true;
      }
      if (open_status != ERROR_SUCCESS) {
        if (log_errors) {
          BOOST_LOG(warning) << "Vulkan HDR bridge: failed to open implicit-layer registry key for cleanup: " << open_status;
        }
        return false;
      }

      std::vector<std::wstring> stale_values;
      for (DWORD index = 0;; ++index) {
        std::wstring name(32768, L'\0');
        DWORD name_length = static_cast<DWORD>(name.size());
        const LONG status = RegEnumValueW(key, index, name.data(), &name_length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (status != ERROR_SUCCESS) {
          RegCloseKey(key);
          if (log_errors) {
            BOOST_LOG(warning) << "Vulkan HDR bridge: failed to enumerate implicit layers: " << status;
          }
          return false;
        }
        name.resize(name_length);
        if (is_bridge_manifest_value(name)) {
          stale_values.emplace_back(std::move(name));
        }
      }

      bool success = true;
      for (const auto &value : stale_values) {
        const LONG status = RegDeleteValueW(key, value.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
          if (log_errors) {
            BOOST_LOG(warning) << "Vulkan HDR bridge: failed to remove implicit-layer value: " << status;
          }
          success = false;
        }
      }
      RegCloseKey(key);
      return success;
    }

    std::wstring
    active_zako_display() {
      UINT32 path_count = 0;
      UINT32 mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return {};
      }
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
        return {};
      }

      for (UINT32 index = 0; index < path_count; ++index) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[index].targetInfo.adapterId;
        target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
          continue;
        }
        const auto friendly = lowercase(target.monitorFriendlyDeviceName);
        const auto device_path = lowercase(target.monitorDevicePath);
        if (friendly.find(L"zako") == std::wstring::npos && device_path.find(L"#zak") == std::wstring::npos) {
          continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
          return source.viewGdiDeviceName;
        }
      }
      return {};
    }

    bool
    run_helper(const std::filesystem::path &probe, const std::string &arguments,
               const char *operation, bool log_errors = true) {
      const std::string command = '"' + probe.string() + "\" " + arguments;
      boost::process::v1::environment environment = boost::this_process::environment();
      boost::filesystem::path working_directory = probe.parent_path().string();
      std::error_code error;
      auto child = platf::run_command(true, false, command, working_directory, environment, nullptr, error, nullptr);
      if (error || !child.valid()) {
        if (log_errors) BOOST_LOG(warning) << "Vulkan HDR bridge: failed to launch " << operation << ": " << error.message();
        return false;
      }
      child.wait(error);
      if (error) {
        if (log_errors) BOOST_LOG(warning) << "Vulkan HDR bridge: " << operation << " wait failed: " << error.message();
        return false;
      }
      if (const auto exit_code = child.exit_code(); exit_code != 0) {
        if (log_errors) BOOST_LOG(warning) << "Vulkan HDR bridge: " << operation << " exited with code " << exit_code;
        return false;
      }
      return true;
    }

    bool
    run_probe(const std::filesystem::path &probe, const std::wstring &display) {
      const std::string arguments = "--display \"" + std::filesystem::path(display).string() +
                                    "\" --present scrgb --frames 3 --cache "
                                    "\"%LOCALAPPDATA%\\Sunshine\\" + std::filesystem::path(kCacheName).string() + "\"";
      return run_helper(probe, arguments, "validation probe");
    }

    bool
    register_user_manifest(const std::filesystem::path &probe, const std::filesystem::path &manifest) {
      return run_helper(probe, "--register-implicit-layer \"" + manifest.string() + "\"", "per-user layer registration");
    }

    bool
    unregister_user_manifest(bool log_errors = true) {
      const auto probe = artifact_directory() / kProbeName;
      if (!std::filesystem::exists(probe)) return true;
      return run_helper(probe, "--unregister-implicit-layer \"" + std::filesystem::path(kManifestName).string() +
                               "\"", "per-user layer cleanup", log_errors);
    }

    bool
    enable_locked() {
      if (registered) return true;

      set_status("validating", "Validating Vulkan HDR presentation on the active VDD.");
      const auto directory = artifact_directory();
      const auto manifest = directory / kManifestName;
      const auto layer_dll = directory / kLayerDllName;
      const auto probe = directory / kProbeName;
      if (!std::filesystem::exists(manifest) || !std::filesystem::exists(layer_dll) || !std::filesystem::exists(probe)) {
        BOOST_LOG(warning) << "Vulkan HDR bridge artifacts are not installed under " << directory.string();
        set_status("unavailable", "Vulkan HDR bridge files are not installed.");
        return false;
      }

      remove_machine_bridge_values();
      unregister_user_manifest();
      const auto display = active_zako_display();
      if (display.empty()) {
        BOOST_LOG(warning) << "Vulkan HDR bridge: no active Zako display was found";
        set_status("error", "No active Zako HDR display was found.");
        return false;
      }
      if (!run_probe(probe, display)) {
        set_status("error", "Vulkan HDR presentation validation failed. The stream continues without the workaround.");
        return false;
      }
      if (!register_user_manifest(probe, manifest)) {
        set_status("error", "Vulkan layer registration failed. The stream continues without the workaround.");
        return false;
      }

      registered = true;
      set_status("enabled", "Vulkan HDR compatibility is active for this VDD stream.");
      return true;
    }
  }  // namespace

  void
  startup_cleanup() {
    std::lock_guard lock(state_mutex);
    const bool machine_cleaned = remove_machine_bridge_values();
    const bool user_cleaned = unregister_user_manifest();
    const bool cleaned = machine_cleaned && user_cleaned;
    registered = false;
    set_status(cleaned ? "idle" : "error",
      cleaned ? "Waiting for an HDR VDD stream." : "Could not clean a stale Vulkan layer registration.");
  }

  bool
  enable_for_vdd_hdr_session() {
    std::lock_guard lock(state_mutex);
    const bool enabled = enable_locked();
    if (enabled) BOOST_LOG(info) << "Vulkan HDR bridge enabled for the active VDD HDR session";
    return enabled;
  }

  bool
  validate_now() {
    std::lock_guard lock(state_mutex);
    if (registered) return true;
    if (!enable_locked()) return false;

    const bool machine_cleaned = remove_machine_bridge_values();
    const bool user_cleaned = unregister_user_manifest();
    registered = false;
    if (!machine_cleaned || !user_cleaned) {
      set_status("error", "Validation succeeded, but the temporary Vulkan layer registration could not be removed.");
      return false;
    }
    set_status("idle", "Validation succeeded. Ready for an HDR VDD stream.");
    return true;
  }

  void
  disable() {
    std::lock_guard lock(state_mutex);
    if (!registered) {
      return;
    }
    const bool machine_cleaned = remove_machine_bridge_values();
    const bool user_cleaned = unregister_user_manifest();
    const bool cleaned = machine_cleaned && user_cleaned;
    if (cleaned && registered) {
      BOOST_LOG(info) << "Vulkan HDR bridge disabled";
    }
    registered = false;
    set_status(cleaned ? "idle" : "error",
      cleaned ? "Waiting for an HDR VDD stream." : "Could not remove the Vulkan layer registration.");
  }

  void
  shutdown_cleanup() {
    std::lock_guard lock(state_mutex);
    if (!registered) {
      return;
    }
    remove_machine_bridge_values(false);
    unregister_user_manifest(false);
    registered = false;
  }

  status_t
  status() {
    std::lock_guard lock(state_mutex);
    return { current_state, current_message, registered, artifacts_installed() };
  }

}  // namespace platf::vulkan_hdr_bridge
