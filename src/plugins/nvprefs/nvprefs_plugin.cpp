/**
 * @file src/plugins/nvprefs/nvprefs_plugin.cpp
 * @brief Official NVIDIA Control Panel lifecycle plugin entry point.
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <cwctype>

#include <nlohmann/json.hpp>

#include "src/platform/windows/nvprefs/nvprefs_common.h"
#include "src/platform/windows/nvprefs/nvprefs_interface.h"

using namespace std::literals;

namespace {

  constexpr int supported_protocol_version = 1;

  enum exit_code_e {
    success = 0,
    plugin_error = 1,
    config_error = 2,
    retry_requested = 3,
  };

  struct cli_options_t {
    std::optional<std::string> event;
    std::optional<std::filesystem::path> payload_path;
    std::optional<std::filesystem::path> result_path;
    bool help = false;
  };

  void
  print_usage(const char *program) {
    std::cout << "Usage: " << program << " --event <slot> --payload <payload.json> [--result <result.json>]\n";
  }

  void
  log_message(std::string_view message) {
    std::cerr << "sunshine-plugin-nvprefs: " << message << '\n';
  }

  std::string result_stage;
  std::string result_message;

  void
  set_result(std::string_view stage, std::string_view message) {
    result_stage = std::string { stage };
    result_message = std::string { message };
  }

  int
  fail_step(std::string_view stage, std::string_view message) {
    set_result(stage, message);
    log_message(message);
    return plugin_error;
  }

  std::optional<std::string>
  next_value(int &index, int argc, char **argv, std::string_view option) {
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << option << '\n';
      return std::nullopt;
    }

    ++index;
    return argv[index];
  }

  cli_options_t
  parse_cli(int argc, char **argv) {
    cli_options_t options;

    for (int i = 1; i < argc; ++i) {
      const std::string_view arg { argv[i] };
      if (arg == "--help"sv || arg == "-h"sv) {
        options.help = true;
      }
      else if (arg == "--event"sv) {
        options.event = next_value(i, argc, argv, arg);
      }
      else if (arg == "--payload"sv) {
        if (auto value = next_value(i, argc, argv, arg)) {
          options.payload_path = std::filesystem::path { *value };
        }
      }
      else if (arg == "--result"sv) {
        if (auto value = next_value(i, argc, argv, arg)) {
          options.result_path = std::filesystem::path { *value };
        }
      }
      else {
        std::cerr << "Unknown argument: " << arg << '\n';
        options.help = true;
      }
    }

    return options;
  }

  void
  write_result(const std::optional<std::filesystem::path> &path, std::string_view event, int exit_code) {
    if (!path) {
      return;
    }

    try {
      nlohmann::json result {
        {"status", exit_code == success ? "success" : "failed"},
        {"event", std::string { event }},
        {"message", !result_message.empty() ? result_message :
                      exit_code == success ?
                      "NVIDIA Control Panel optimizer completed." :
                      "NVIDIA Control Panel optimizer failed."},
      };
      if (!result_stage.empty()) {
        result["stage"] = result_stage;
      }

      std::ofstream file(*path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) {
        std::cerr << "Could not open result path: " << *path << '\n';
        return;
      }
      file << result.dump(2);
    }
    catch (const std::exception &err) {
      std::cerr << "Could not write result: " << err.what() << '\n';
    }
  }

  std::optional<nlohmann::json>
  read_payload(const std::filesystem::path &path) {
    try {
      std::ifstream file(path);
      if (!file.is_open()) {
        std::cerr << "Could not open payload: " << path << '\n';
        return std::nullopt;
      }

      return nlohmann::json::parse(file);
    }
    catch (const std::exception &err) {
      std::cerr << "Could not parse payload: " << err.what() << '\n';
      return std::nullopt;
    }
  }

  bool
  protocol_supported(const nlohmann::json &payload) {
    if (!payload.contains("host") || !payload.at("host").is_object()) {
      log_message("payload is missing host metadata");
      return false;
    }

    const auto &host = payload.at("host");
    if (!host.contains("protocol_version") || !host.at("protocol_version").is_number_integer()) {
      log_message("payload is missing host.protocol_version");
      return false;
    }

    const auto protocol_version = host.at("protocol_version").get<int>();
    if (protocol_version != supported_protocol_version) {
      std::cerr << "Unsupported protocol version: " << protocol_version << '\n';
      return false;
    }

    return true;
  }

  bool
  event_matches_payload(std::string_view event, const nlohmann::json &payload) {
    if (!payload.contains("event") || !payload.at("event").is_string()) {
      log_message("payload is missing event");
      return false;
    }

    const auto payload_event = payload.at("event").get<std::string>();
    if (payload_event != event) {
      std::cerr << "Event argument does not match payload: " << event << " != " << payload_event << '\n';
      return false;
    }

    return true;
  }

  bool
  read_bool_config(const nlohmann::json &config, const char *key, bool fallback) {
    if (config.contains(key) && config.at(key).is_boolean()) {
      return config.at(key).get<bool>();
    }

    return fallback;
  }

  int
  read_int_config(const nlohmann::json &config, const char *key, int fallback) {
    if (config.contains(key) && config.at(key).is_number_integer()) {
      return config.at(key).get<int>();
    }

    return fallback;
  }

  int
  clamp_int(int value, int min, int max) {
    if (value < min) {
      return min;
    }
    if (value > max) {
      return max;
    }
    return value;
  }

  const nlohmann::json *
  plugin_config_from_payload(const nlohmann::json &payload) {
    const auto plugin = payload.find("plugin");
    if (plugin == payload.end() || !plugin->is_object()) {
      return nullptr;
    }

    const auto config = plugin->find("config");
    if (config == plugin->end() || !config->is_object()) {
      return nullptr;
    }

    return &*config;
  }

  nvprefs::nvprefs_options
  options_from_payload(const nlohmann::json &payload) {
    nvprefs::nvprefs_options options;
    const auto config = plugin_config_from_payload(payload);
    if (!config) {
      return options;
    }

    options.opengl_vulkan_on_dxgi = read_bool_config(*config, "opengl_vulkan_on_dxgi", options.opengl_vulkan_on_dxgi);
    options.sunshine_high_power_mode = read_bool_config(*config, "sunshine_high_power_mode", options.sunshine_high_power_mode);
    options.nv_optimize_game = read_bool_config(*config, "nv_optimize_game", options.nv_optimize_game);
    options.nv_force_vsync = read_bool_config(*config, "nv_force_vsync", options.nv_force_vsync);
    options.nv_lock_frame_rate = read_bool_config(*config, "nv_lock_frame_rate", options.nv_lock_frame_rate);
    options.nv_frl_fps_offset = clamp_int(read_int_config(*config, "nv_frl_fps_offset", options.nv_frl_fps_offset), -30, 30);
    options.nv_frl_fps_override = clamp_int(read_int_config(*config, "nv_frl_fps_override", options.nv_frl_fps_override), 0, 500);
    options.nv_prefer_max_performance = read_bool_config(*config, "nv_prefer_max_performance", options.nv_prefer_max_performance);
    options.nv_low_latency_mode = read_bool_config(*config, "nv_low_latency_mode", options.nv_low_latency_mode);
    options.nv_apply_to_base_profile = read_bool_config(*config, "nv_apply_to_base_profile", options.nv_apply_to_base_profile);
    return options;
  }

  bool
  config_bool_from_payload(const nlohmann::json &payload, const char *key, bool fallback) {
    const auto config = plugin_config_from_payload(payload);
    if (!config) {
      return fallback;
    }

    return read_bool_config(*config, key, fallback);
  }

  void
  apply_options(const nlohmann::json &payload) {
    nvprefs::set_nvprefs_options(options_from_payload(payload));
  }

  std::wstring
  wide_from_utf8(const std::string &value) {
    if (value.empty()) {
      return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
      return {};
    }

    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
  }

  std::wstring
  lower_copy(std::wstring value) {
    for (auto &ch : value) {
      ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
  }

  std::wstring
  extract_exe_basename_from_command(const std::string &command) {
    const std::wstring wide_command = wide_from_utf8(command);
    const auto first = wide_command.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
      return {};
    }

    std::wstring token;
    const wchar_t quote = wide_command[first];
    if (quote == L'"' || quote == L'\'') {
      const auto close = wide_command.find(quote, first + 1);
      token = wide_command.substr(first + 1, close == std::wstring::npos ? std::wstring::npos : close - first - 1);
    }
    else {
      const auto end = wide_command.find_first_of(L" \t\r\n", first);
      token = wide_command.substr(first, end == std::wstring::npos ? std::wstring::npos : end - first);
    }

    if (token.empty()) {
      return {};
    }

    const auto filename = std::filesystem::path(token).filename().wstring();
    return lower_copy(filename.empty() ? token : filename);
  }

  std::wstring
  app_exe_name_from_payload(const nlohmann::json &payload) {
    const auto app = payload.find("app");
    if (app == payload.end() || !app->is_object()) {
      return {};
    }

    const auto cmd = app->find("cmd");
    if (cmd == app->end() || !cmd->is_string()) {
      return {};
    }

    return extract_exe_basename_from_command(cmd->get<std::string>());
  }

  int
  client_fps_from_payload(const nlohmann::json &payload) {
    const auto session = payload.find("session");
    if (session == payload.end() || !session->is_object()) {
      return 60;
    }

    const auto fps = session->find("client_fps");
    if (fps == session->end() || !fps->is_number()) {
      return 60;
    }

    const int value = fps->is_number_integer() ? fps->get<int>() : static_cast<int>(fps->get<double>() + 0.5);
    return value > 0 ? value : 60;
  }

  bool
  load_or_skip(nvprefs::nvprefs_interface &preferences) {
    if (preferences.load()) {
      set_result("load", "NvAPI loaded.");
      return true;
    }

    set_result("load", "NvAPI is not available; NVIDIA profile work was skipped.");
    log_message(result_message);
    return false;
  }

  int
  restore_from_undo_file(const nlohmann::json &payload) {
    apply_options(payload);

    nvprefs::nvprefs_interface preferences;
    if (!load_or_skip(preferences)) {
      return success;
    }

    const auto restored = preferences.restore_from_and_delete_undo_file_if_exists();
    preferences.unload();
    if (!restored) {
      return fail_step("restore_undo", "Failed to restore NVIDIA driver settings from the undo file.");
    }

    set_result("restore_undo", "NVIDIA driver settings recovery completed.");
    return success;
  }

  int
  apply_stream_profile(const nlohmann::json &payload) {
    apply_options(payload);

    nvprefs::nvprefs_interface preferences;
    if (!load_or_skip(preferences)) {
      return success;
    }

    if (!preferences.restore_from_and_delete_undo_file_if_exists()) {
      preferences.unload();
      return fail_step("restore_undo", "Failed to restore pending NVIDIA driver settings before applying stream optimization.");
    }

    if (!preferences.modify_application_profile()) {
      preferences.unload();
      return fail_step("modify_sunshine_profile", "Failed to update the Sunshine NVIDIA application profile.");
    }

    if (!preferences.apply_stream_optimizations(app_exe_name_from_payload(payload), client_fps_from_payload(payload))) {
      preferences.unload();
      return fail_step("apply_stream_profile", "Failed to apply NVIDIA stream optimization to the game profile.");
    }

    if (!preferences.modify_global_profile()) {
      preferences.unload();
      return fail_step("modify_global_profile", "Failed to update the NVIDIA global profile.");
    }

    if (preferences.owning_undo_file()) {
      preferences.release_undo_file_for_later_restore();
    }

    preferences.unload();
    set_result("apply_stream_profile", "NVIDIA stream optimization was applied.");
    return success;
  }

  int
  handle_startup_recover(const nlohmann::json &payload) {
    return restore_from_undo_file(payload);
  }

  int
  handle_first_session_starting(const nlohmann::json &payload) {
    return apply_stream_profile(payload);
  }

  int
  handle_dynamic_params_changed(const nlohmann::json &payload) {
    if (!config_bool_from_payload(payload, "dynamic_stream_params", false)) {
      log_message("dynamic params slot received; plugin config keeps it as a no-op");
      return success;
    }

    return apply_stream_profile(payload);
  }

  int
  handle_last_session_stopping(const nlohmann::json &payload) {
    return restore_from_undo_file(payload);
  }

  int
  handle_shutdown_restoring(const nlohmann::json &payload) {
    return restore_from_undo_file(payload);
  }

  int
  dispatch_event(std::string_view event, const nlohmann::json &payload) {
    if (event == "sunshine.startup.recover"sv) {
      return handle_startup_recover(payload);
    }
    if (event == "stream.first_session.starting"sv) {
      return handle_first_session_starting(payload);
    }
    if (event == "stream.dynamic_params.changed"sv) {
      return handle_dynamic_params_changed(payload);
    }
    if (event == "stream.last_session.stopping"sv) {
      return handle_last_session_stopping(payload);
    }
    if (event == "sunshine.shutdown.restoring"sv) {
      return handle_shutdown_restoring(payload);
    }

    std::cerr << "Unsupported event: " << event << '\n';
    return config_error;
  }

}  // namespace

int
main(int argc, char **argv) {
  const auto options = parse_cli(argc, argv);
  if (options.help || !options.event || !options.payload_path) {
    print_usage(argv[0]);
    return config_error;
  }

  const auto payload = read_payload(*options.payload_path);
  if (!payload) {
    return config_error;
  }

  if (!protocol_supported(*payload) || !event_matches_payload(*options.event, *payload)) {
    return config_error;
  }

  const auto exit_code = dispatch_event(*options.event, *payload);
  write_result(options.result_path, *options.event, exit_code);
  return exit_code;
}
