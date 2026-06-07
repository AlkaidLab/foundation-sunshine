/**
 * @file src/plugin.cpp
 * @brief Lifecycle plugin host definitions.
 */

#include "plugin.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>

#include "config.h"
#include "httpcommon.h"
#include "logging.h"
#include "platform/run_command.h"
#include "utility.h"

using namespace std::literals;

namespace bp = boost::process::v1;
namespace fs = std::filesystem;

namespace {

  constexpr int protocol_version = 1;
  constexpr int default_timeout_ms = 5000;
  constexpr std::size_t max_history_entries = 20;
  constexpr std::size_t ui_history_entries = 8;
  constexpr std::string_view default_marketplace_index_url = "https://alkaidlab.github.io/sunshine-plugin-registry/index.json";

  struct plugin_action_t {
    std::string id;
    std::string title;
    std::string description;
    std::string event;
    std::string icon;
    bool danger = false;
  };

  struct manifest_t {
    int api_version = 1;
    std::string id;
    std::string name;
    std::string version;
    std::string min_host_version;
    fs::path root_dir;
    fs::path entry_path;
    std::string config_schema;
    nlohmann::json default_config = nlohmann::json::object();
    nlohmann::json package = nlohmann::json::object();
    std::vector<plugin_action_t> actions;
    std::set<std::string> slots;
    std::set<std::string> platforms;
    std::set<std::string> permissions;
    std::set<std::string> capabilities;
    std::chrono::milliseconds timeout { default_timeout_ms };
    bool enabled = true;
  };

  struct plugin_config_t {
    nlohmann::json config = nlohmann::json::object();
    bool has_user_config = false;
  };

  std::mutex config_write_mutex;
  std::mutex history_mutex;

  std::string_view
  current_platform() {
#if defined(_WIN32)
    return "windows"sv;
#elif defined(__APPLE__)
    return "macos"sv;
#elif defined(__linux__)
    return "linux"sv;
#else
    return "unknown"sv;
#endif
  }

  bool
  json_string_array_to_set(const nlohmann::json &j, const char *field, std::set<std::string> &out) {
    if (!j.contains(field)) {
      return false;
    }
    if (!j.at(field).is_array()) {
      BOOST_LOG(warning) << "Plugin manifest field '" << field << "' is not an array";
      return false;
    }

    for (const auto &item : j.at(field)) {
      if (!item.is_string()) {
        BOOST_LOG(warning) << "Plugin manifest field '" << field << "' contains a non-string value";
        continue;
      }
      out.insert(item.get<std::string>());
    }

    return true;
  }

  void
  json_actions_to_vector(const nlohmann::json &j, std::vector<plugin_action_t> &out) {
    if (!j.contains("actions")) {
      return;
    }
    if (!j.at("actions").is_array()) {
      BOOST_LOG(warning) << "Plugin manifest field 'actions' is not an array";
      return;
    }

    for (const auto &item : j.at("actions")) {
      if (!item.is_object()) {
        BOOST_LOG(warning) << "Plugin manifest action entry is not an object";
        continue;
      }
      if (!item.contains("id") || !item.at("id").is_string() ||
          !item.contains("event") || !item.at("event").is_string()) {
        BOOST_LOG(warning) << "Plugin manifest action is missing string id or event";
        continue;
      }

      plugin_action_t action;
      action.id = item.at("id").get<std::string>();
      action.event = item.at("event").get<std::string>();
      action.title = item.contains("title") && item.at("title").is_string() ?
                       item.at("title").get<std::string>() :
                       action.id;
      if (item.contains("description") && item.at("description").is_string()) {
        action.description = item.at("description").get<std::string>();
      }
      if (item.contains("icon") && item.at("icon").is_string()) {
        action.icon = item.at("icon").get<std::string>();
      }
      if (item.contains("danger") && item.at("danger").is_boolean()) {
        action.danger = item.at("danger").get<bool>();
      }

      out.push_back(std::move(action));
    }
  }

  bool
  platform_supported(const manifest_t &manifest) {
    return manifest.platforms.empty() || manifest.platforms.contains(std::string { current_platform() });
  }

  bool
  platform_supported(const nlohmann::json &platforms) {
    if (!platforms.is_array() || platforms.empty()) {
      return true;
    }

    const auto platform = current_platform();
    return std::any_of(platforms.begin(), platforms.end(), [platform](const auto &item) {
      return item.is_string() && item.template get<std::string>() == platform;
    });
  }

  std::string
  marketplace_index_url() {
    if (const auto *override_url = std::getenv("SUNSHINE_PLUGIN_MARKETPLACE_INDEX_URL")) {
      if (override_url[0] != '\0') {
        return override_url;
      }
    }

    return std::string { default_marketplace_index_url };
  }

  std::optional<fs::path>
  config_plugins_root() {
    if (config::sunshine.config_file.empty()) {
      return std::nullopt;
    }

    const auto config_dir = fs::path(config::sunshine.config_file).parent_path();
    if (config_dir.empty()) {
      return std::nullopt;
    }

    return config_dir / "plugins";
  }

  std::optional<fs::path>
  config_plugin_root(std::string_view id) {
    if (auto root = config_plugins_root()) {
      return *root / std::string { id };
    }

    return std::nullopt;
  }

  std::optional<fs::path>
  config_plugin_config_path(std::string_view id) {
    if (auto root = config_plugin_root(id)) {
      return *root / "config.json";
    }

    return std::nullopt;
  }

  std::optional<fs::path>
  config_plugin_state_path(std::string_view id) {
    if (auto root = config_plugin_root(id)) {
      return *root / "state.json";
    }

    return std::nullopt;
  }

  std::optional<fs::path>
  config_plugin_history_path(std::string_view id) {
    if (auto root = config_plugin_root(id)) {
      return *root / "history.json";
    }

    return std::nullopt;
  }

  std::optional<manifest_t>
  load_manifest(const fs::path &manifest_path) {
    try {
      std::ifstream file(manifest_path);
      if (!file.is_open()) {
        BOOST_LOG(warning) << "Plugin manifest could not be opened: " << manifest_path;
        return std::nullopt;
      }

      const auto root = nlohmann::json::parse(file);
      manifest_t manifest;
      manifest.root_dir = manifest_path.parent_path();

      if (!root.contains("id") || !root.at("id").is_string()) {
        BOOST_LOG(warning) << "Plugin manifest missing string id: " << manifest_path;
        return std::nullopt;
      }
      if (!root.contains("entry") || !root.at("entry").is_string()) {
        BOOST_LOG(warning) << "Plugin manifest missing string entry: " << manifest_path;
        return std::nullopt;
      }

      manifest.id = root.at("id").get<std::string>();
      manifest.entry_path = manifest.root_dir / root.at("entry").get<std::string>();

      if (root.contains("api_version") && root.at("api_version").is_number_integer()) {
        manifest.api_version = root.at("api_version").get<int>();
      }

      if (root.contains("name") && root.at("name").is_string()) {
        manifest.name = root.at("name").get<std::string>();
      }
      else {
        manifest.name = manifest.id;
      }

      if (root.contains("version") && root.at("version").is_string()) {
        manifest.version = root.at("version").get<std::string>();
      }

      if (root.contains("min_host_version") && root.at("min_host_version").is_string()) {
        manifest.min_host_version = root.at("min_host_version").get<std::string>();
      }

      if (root.contains("package") && root.at("package").is_object()) {
        manifest.package = root.at("package");
      }

      if (root.contains("config_schema") && root.at("config_schema").is_string()) {
        manifest.config_schema = root.at("config_schema").get<std::string>();
      }

      if (root.contains("enabled") && root.at("enabled").is_boolean()) {
        manifest.enabled = root.at("enabled").get<bool>();
      }

      if (root.contains("default_config") && root.at("default_config").is_object()) {
        manifest.default_config = root.at("default_config");
      }

      json_string_array_to_set(root, "slots", manifest.slots);
      json_string_array_to_set(root, "platforms", manifest.platforms);
      json_string_array_to_set(root, "permissions", manifest.permissions);
      json_string_array_to_set(root, "capabilities", manifest.capabilities);
      json_actions_to_vector(root, manifest.actions);

      if (manifest.slots.empty() && manifest.actions.empty()) {
        BOOST_LOG(warning) << "Plugin manifest has no lifecycle slots or actions: " << manifest_path;
        return std::nullopt;
      }

      if (root.contains("timeout_ms") && root.at("timeout_ms").is_number_integer()) {
        const auto timeout_ms = root.at("timeout_ms").get<int>();
        if (timeout_ms > 0) {
          manifest.timeout = std::chrono::milliseconds { timeout_ms };
        }
      }

      return manifest;
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Plugin manifest parse failed for " << manifest_path << ": " << err.what();
      return std::nullopt;
    }
  }

  bool
  same_path(const fs::path &lhs, const fs::path &rhs) {
    return lhs.lexically_normal().wstring() == rhs.lexically_normal().wstring();
  }

  bool
  merge_json_object_file(const fs::path &path, nlohmann::json &target) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      return false;
    }

    try {
      std::ifstream file(path);
      if (!file.is_open()) {
        BOOST_LOG(warning) << "Plugin config could not be opened: " << path;
        return false;
      }

      const auto overlay = nlohmann::json::parse(file);
      if (!overlay.is_object()) {
        BOOST_LOG(warning) << "Plugin config is not an object: " << path;
        return false;
      }

      for (const auto &[key, value] : overlay.items()) {
        target[key] = value;
      }
      return true;
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Plugin config parse failed for " << path << ": " << err.what();
      return false;
    }
  }

  void
  apply_user_state(manifest_t &manifest) {
    const auto state_path = config_plugin_state_path(manifest.id);
    if (!state_path) {
      return;
    }

    std::error_code ec;
    if (!fs::exists(*state_path, ec)) {
      return;
    }

    try {
      std::ifstream file(*state_path);
      if (!file.is_open()) {
        BOOST_LOG(warning) << "Plugin state could not be opened: " << *state_path;
        return;
      }

      const auto state = nlohmann::json::parse(file);
      if (state.contains("enabled") && state.at("enabled").is_boolean()) {
        manifest.enabled = state.at("enabled").get<bool>();
      }
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Plugin state parse failed for " << *state_path << ": " << err.what();
    }
  }

  void
  scan_plugin_root(const fs::path &root, std::map<std::string, manifest_t> &plugins) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
      return;
    }

    for (const auto &entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        BOOST_LOG(warning) << "Plugin directory scan failed for " << root << ": " << ec.message();
        return;
      }
      if (!entry.is_directory(ec)) {
        continue;
      }

      if (auto manifest = load_manifest(entry.path() / "plugin.json")) {
        plugins[manifest->id] = std::move(*manifest);
      }
    }
  }

  std::vector<manifest_t>
  load_installed_plugins() {
    std::map<std::string, manifest_t> plugins;

    scan_plugin_root(fs::path(SUNSHINE_ASSETS_DIR) / "plugins", plugins);

    if (auto root = config_plugins_root()) {
      scan_plugin_root(*root, plugins);
    }

    std::vector<manifest_t> result;
    result.reserve(plugins.size());
    for (auto &[_, manifest] : plugins) {
      apply_user_state(manifest);
      result.emplace_back(std::move(manifest));
    }

    return result;
  }

  std::string
  quote_command_arg(const std::string &arg) {
    if (arg.empty()) {
      return "\"\"";
    }

    bool needs_quotes = false;
    for (char ch : arg) {
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == '"') {
        needs_quotes = true;
        break;
      }
    }

    if (!needs_quotes) {
      return arg;
    }

    std::string quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back('"');
    for (char ch : arg) {
      if (ch == '"' || ch == '\\') {
        quoted.push_back('\\');
      }
      quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
  }

  std::string
  sanitize_filename_part(std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
      sanitized.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? ch : '-');
    }
    return sanitized;
  }

  std::optional<fs::path>
  write_payload_file(std::string_view event_name, const nlohmann::json &payload) {
    try {
      const auto temp_dir = fs::temp_directory_path() / "Sunshine" / "plugins";
      std::error_code ec;
      fs::create_directories(temp_dir, ec);
      if (ec) {
        BOOST_LOG(warning) << "Could not create plugin payload directory " << temp_dir << ": " << ec.message();
        return std::nullopt;
      }

      const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
      const auto payload_path = temp_dir / ("payload-" + sanitize_filename_part(event_name) + "-" + std::to_string(ticks) + ".json");

      std::ofstream file(payload_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) {
        BOOST_LOG(warning) << "Could not write plugin payload: " << payload_path;
        return std::nullopt;
      }

      file << payload.dump(2);
      return payload_path;
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Could not create plugin payload: " << err.what();
      return std::nullopt;
    }
  }

  std::optional<fs::path>
  reserve_result_file(std::string_view event_name) {
    try {
      const auto temp_dir = fs::temp_directory_path() / "Sunshine" / "plugins";
      std::error_code ec;
      fs::create_directories(temp_dir, ec);
      if (ec) {
        BOOST_LOG(warning) << "Could not create plugin result directory " << temp_dir << ": " << ec.message();
        return std::nullopt;
      }

      const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
      return temp_dir / ("result-" + sanitize_filename_part(event_name) + "-" + std::to_string(ticks) + ".json");
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Could not reserve plugin result path: " << err.what();
      return std::nullopt;
    }
  }

  plugin_config_t
  load_plugin_config(const manifest_t &manifest) {
    plugin_config_t result {
      manifest.default_config.is_object() ? manifest.default_config : nlohmann::json::object(),
      false,
    };
    const auto root_config_path = manifest.root_dir / "config.json";
    auto user_config_path = config_plugin_config_path(manifest.id);

    if (merge_json_object_file(root_config_path, result.config)) {
      result.has_user_config = true;
    }

    if (user_config_path && !same_path(root_config_path, *user_config_path) &&
        merge_json_object_file(*user_config_path, result.config)) {
      result.has_user_config = true;
    }

    return result;
  }

  void
  apply_legacy_plugin_config(const manifest_t &manifest, plugin_config_t &plugin_config) {
    if (plugin_config.has_user_config) {
      return;
    }

#if defined(_WIN32)
    if (manifest.id == "com.alkaidlab.nvidia-control-panel-optimizer") {
      plugin_config.config["opengl_vulkan_on_dxgi"] = config::video.nv_opengl_vulkan_on_dxgi;
      plugin_config.config["sunshine_high_power_mode"] = config::video.nv_sunshine_high_power_mode;
    }
#endif
  }

  void
  add_plugin_payload(nlohmann::json &payload, const manifest_t &manifest) {
    auto plugin_config = load_plugin_config(manifest);
    apply_legacy_plugin_config(manifest, plugin_config);

    payload["plugin"] = {
      {"id", manifest.id},
      {"name", manifest.name},
      {"version", manifest.version},
      {"root_dir", manifest.root_dir.string()},
      {"config", plugin_config.config},
    };
  }

  bool
  entry_exists(const manifest_t &manifest) {
    std::error_code ec;
    return fs::exists(manifest.entry_path, ec);
  }

  nlohmann::json
  set_to_json_array(const std::set<std::string> &values) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto &value : values) {
      result.emplace_back(value);
    }
    return result;
  }

  nlohmann::json
  actions_to_json_array(const std::vector<plugin_action_t> &actions) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto &action : actions) {
      nlohmann::json item {
        {"id", action.id},
        {"title", action.title},
        {"event", action.event},
        {"danger", action.danger},
      };
      if (!action.description.empty()) {
        item["description"] = action.description;
      }
      if (!action.icon.empty()) {
        item["icon"] = action.icon;
      }
      result.push_back(std::move(item));
    }
    return result;
  }

  std::optional<nlohmann::json>
  read_json_file(const fs::path &path) {
    try {
      std::ifstream file(path);
      if (!file.is_open()) {
        return std::nullopt;
      }

      return nlohmann::json::parse(file);
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "JSON file parse failed for " << path << ": " << err.what();
      return std::nullopt;
    }
  }

  std::int64_t
  unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
  }

  nlohmann::json
  read_plugin_history_unlocked(std::string_view id, std::size_t limit) {
    nlohmann::json result = nlohmann::json::array();
    const auto history_path = config_plugin_history_path(id);
    if (!history_path) {
      return result;
    }

    const auto history = read_json_file(*history_path);
    if (!history || !history->is_array()) {
      return result;
    }

    const auto count = std::min<std::size_t>(history->size(), limit);
    for (std::size_t i = 0; i < count; ++i) {
      result.push_back((*history)[i]);
    }

    return result;
  }

  nlohmann::json
  read_plugin_history(std::string_view id, std::size_t limit) {
    std::lock_guard lock(history_mutex);
    return read_plugin_history_unlocked(id, limit);
  }

  void
  append_plugin_history(std::string_view id, const nlohmann::json &record) {
    std::lock_guard lock(history_mutex);
    const auto history_path = config_plugin_history_path(id);
    if (!history_path) {
      BOOST_LOG(warning) << "Plugin history path is unavailable for '" << id << "'";
      return;
    }

    nlohmann::json history = nlohmann::json::array();
    history.push_back(record);

    const auto previous = read_plugin_history_unlocked(id, max_history_entries);
    for (std::size_t i = 0; i < previous.size() && history.size() < max_history_entries; ++i) {
      history.push_back(previous[i]);
    }

    std::error_code ec;
    fs::create_directories(history_path->parent_path(), ec);
    if (ec) {
      BOOST_LOG(warning) << "Could not create plugin history directory: " << ec.message();
      return;
    }

    try {
      std::ofstream file(*history_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) {
        BOOST_LOG(warning) << "Could not open plugin history file: " << *history_path;
        return;
      }
      file << history.dump(2);
    }
    catch (const std::exception &err) {
      BOOST_LOG(warning) << "Could not write plugin history for '" << id << "': " << err.what();
    }
  }

  std::optional<manifest_t>
  find_plugin(std::string_view id) {
    auto plugins = load_installed_plugins();
    for (auto &manifest : plugins) {
      if (manifest.id == id) {
        return manifest;
      }
    }

    return std::nullopt;
  }

  std::map<std::string, manifest_t>
  installed_plugin_map() {
    std::map<std::string, manifest_t> result;
    for (auto &manifest : load_installed_plugins()) {
      result[manifest.id] = std::move(manifest);
    }
    return result;
  }

  bool
  wait_for_plugin(bp::child &child, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (child.running()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(50ms);
    }
    child.wait();
    return true;
  }

  nlohmann::json
  invoke_plugin(const manifest_t &manifest, std::string_view event_name, const fs::path &payload_path, std::string_view trigger, std::string_view action_id = {}) {
    boost::filesystem::path working_dir { manifest.root_dir.string() };
    auto env = boost::this_process::environment();
    bp::group group;
    std::error_code ec;
    const auto started_at_ms = unix_time_ms();
    const auto started_at = std::chrono::steady_clock::now();

    nlohmann::json record {
      {"plugin_id", manifest.id},
      {"event", std::string { event_name }},
      {"trigger", std::string { trigger }},
      {"entry", manifest.entry_path.filename().string()},
      {"started_at_ms", started_at_ms},
      {"timeout_ms", manifest.timeout.count()},
      {"success", false},
      {"timed_out", false},
    };
    if (!action_id.empty()) {
      record["action_id"] = std::string { action_id };
    }

    const auto finish_record = [&](std::string_view status, bool success) {
      record["status"] = std::string { status };
      record["success"] = success;
      record["finished_at_ms"] = unix_time_ms();
      record["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
      append_plugin_history(manifest.id, record);
      return record;
    };

    const auto result_path = reserve_result_file(event_name);
    auto cleanup_result = util::fail_guard([&]() {
      if (result_path) {
        std::error_code remove_ec;
        fs::remove(*result_path, remove_ec);
      }
    });

    const auto merge_plugin_result = [&]() {
      if (!result_path) {
        return;
      }

      const auto plugin_result = read_json_file(*result_path);
      if (!plugin_result) {
        return;
      }
      if (!plugin_result->is_object()) {
        record["result_error"] = "plugin result is not a JSON object";
        return;
      }

      record["result"] = *plugin_result;
      if (plugin_result->contains("message") && plugin_result->at("message").is_string()) {
        record["message"] = plugin_result->at("message").get<std::string>();
      }
    };

    auto cmd = quote_command_arg(manifest.entry_path.string()) +
               " --event " + quote_command_arg(std::string { event_name }) +
               " --payload " + quote_command_arg(payload_path.string());
    if (result_path) {
      cmd += " --result " + quote_command_arg(result_path->string());
    }

    BOOST_LOG(debug) << "Invoking lifecycle plugin '" << manifest.id << "' for " << event_name;
    auto child = platf::run_command(false, false, cmd, working_dir, env, nullptr, ec, &group);
    if (ec || !child.valid()) {
      BOOST_LOG(warning) << "Lifecycle plugin '" << manifest.id << "' failed to start for "
                         << event_name << ": " << ec.message();
      record["error"] = ec.message();
      record["exit_code"] = nullptr;
      return finish_record("start_error"sv, false);
    }

    if (!wait_for_plugin(child, manifest.timeout)) {
      BOOST_LOG(warning) << "Lifecycle plugin '" << manifest.id << "' timed out after "
                         << manifest.timeout.count() << "ms for " << event_name;
      std::error_code terminate_ec;
      if (group.valid()) {
        group.terminate(terminate_ec);
        group.detach();
      }
      if (child.valid()) {
        child.detach();
      }
      record["exit_code"] = nullptr;
      record["timed_out"] = true;
      return finish_record("timeout"sv, false);
    }

    const auto exit_code = child.exit_code();
    record["exit_code"] = exit_code;
    merge_plugin_result();
    if (exit_code != 0) {
      BOOST_LOG(warning) << "Lifecycle plugin '" << manifest.id << "' returned exit code "
                         << exit_code << " for " << event_name;
    }
    return finish_record(exit_code == 0 ? "success"sv : "failed"sv, exit_code == 0);
  }

  nlohmann::json
  build_payload(std::string_view event_name, const nlohmann::json &context) {
    nlohmann::json payload = context.is_object() ? context : nlohmann::json::object();
    payload["host"] = {
      {"name", "Sunshine"},
      {"protocol_version", protocol_version},
    };
    payload["event"] = event_name;
    payload["paths"] = {
      {"assets_dir", std::string { SUNSHINE_ASSETS_DIR }},
      {"config_file", config::sunshine.config_file},
    };
    if (!config::sunshine.config_file.empty()) {
      payload["paths"]["config_dir"] = fs::path(config::sunshine.config_file).parent_path().string();
    }
    return payload;
  }

  nlohmann::json
  build_payload(plugin::lifecycle_event_e event, const nlohmann::json &context) {
    return build_payload(plugin::to_string(event), context);
  }

}  // namespace

namespace plugin {

  std::string_view
  to_string(lifecycle_event_e event) {
    switch (event) {
      case lifecycle_event_e::sunshine_startup_recover:
        return "sunshine.startup.recover"sv;
      case lifecycle_event_e::sunshine_shutdown_restoring:
        return "sunshine.shutdown.restoring"sv;
      case lifecycle_event_e::stream_first_session_starting:
        return "stream.first_session.starting"sv;
      case lifecycle_event_e::stream_last_session_stopping:
        return "stream.last_session.stopping"sv;
      case lifecycle_event_e::stream_dynamic_params_changed:
        return "stream.dynamic_params.changed"sv;
    }

    return "unknown"sv;
  }

  void
  fire_lifecycle_event(lifecycle_event_e event, const nlohmann::json &context) {
    const auto event_name = to_string(event);
    auto plugins = load_installed_plugins();
    if (plugins.empty()) {
      return;
    }

    for (const auto &manifest : plugins) {
      if (!manifest.enabled || !platform_supported(manifest)) {
        continue;
      }
      if (!manifest.slots.contains(std::string { event_name })) {
        continue;
      }
      if (!entry_exists(manifest)) {
        BOOST_LOG(warning) << "Plugin entry does not exist: " << manifest.entry_path;
        continue;
      }

      auto payload = build_payload(event, context);
      add_plugin_payload(payload, manifest);

      auto payload_path = write_payload_file(event_name, payload);
      if (!payload_path) {
        continue;
      }

      auto cleanup_payload = util::fail_guard([&]() {
        std::error_code ec;
        fs::remove(*payload_path, ec);
      });

      invoke_plugin(manifest, event_name, *payload_path, "lifecycle"sv);
    }
  }

  nlohmann::json
  list_installed_plugins() {
    auto plugins = load_installed_plugins();

    nlohmann::json result;
    result["plugins"] = nlohmann::json::array();
    result["total_plugins"] = plugins.size();

    for (const auto &manifest : plugins) {
      auto plugin_config = load_plugin_config(manifest);
      apply_legacy_plugin_config(manifest, plugin_config);

      const auto supports_platform = platform_supported(manifest);
      const auto has_entry = entry_exists(manifest);

      nlohmann::json item;
      item["api_version"] = manifest.api_version;
      item["id"] = manifest.id;
      item["name"] = manifest.name;
      item["version"] = manifest.version;
      item["min_host_version"] = manifest.min_host_version;
      item["enabled"] = manifest.enabled;
      item["platform_supported"] = supports_platform;
      item["entry_exists"] = has_entry;
      item["runnable"] = manifest.enabled && supports_platform && has_entry;
      item["entry"] = manifest.entry_path.filename().string();
      item["root_dir"] = manifest.root_dir.string();
      item["slots"] = set_to_json_array(manifest.slots);
      item["actions"] = actions_to_json_array(manifest.actions);
      item["platforms"] = set_to_json_array(manifest.platforms);
      item["permissions"] = set_to_json_array(manifest.permissions);
      item["capabilities"] = set_to_json_array(manifest.capabilities);
      item["package"] = manifest.package;
      item["default_config"] = manifest.default_config;
      item["config"] = plugin_config.config;
      item["has_user_config"] = plugin_config.has_user_config;
      item["history"] = read_plugin_history(manifest.id, ui_history_entries);
      item["last_run"] = item["history"].empty() ? nlohmann::json(nullptr) : item["history"][0];

      if (!manifest.config_schema.empty()) {
        item["config_schema"] = manifest.config_schema;
        if (auto schema = read_json_file(manifest.root_dir / manifest.config_schema)) {
          item["schema"] = *schema;
        }
      }

      if (auto config_path = config_plugin_config_path(manifest.id)) {
        item["user_config_path"] = config_path->string();
      }
      if (auto state_path = config_plugin_state_path(manifest.id)) {
        item["user_state_path"] = state_path->string();
      }

      result["plugins"].push_back(std::move(item));
    }

    return result;
  }

  nlohmann::json
  list_marketplace_plugins(std::string &error) {
    const auto index_url = marketplace_index_url();
    std::string content;
    if (!http::fetch_url(index_url, content)) {
      error = "could not fetch plugin marketplace index";
      return {};
    }

    nlohmann::json index;
    try {
      index = nlohmann::json::parse(content);
    }
    catch (const std::exception &err) {
      error = std::string { "plugin marketplace index is not valid JSON: " } + err.what();
      return {};
    }

    if (!index.is_object() || !index.contains("plugins") || !index.at("plugins").is_array()) {
      error = "plugin marketplace index must be an object with a plugins array";
      return {};
    }

    const auto installed = installed_plugin_map();

    nlohmann::json result = index;
    result["registry_url"] = index_url;
    result["platform"] = std::string { current_platform() };

    for (auto &entry : result["plugins"]) {
      if (!entry.is_object()) {
        continue;
      }

      const auto id = entry.contains("id") && entry.at("id").is_string() ? entry.at("id").get<std::string>() : std::string {};
      const auto installed_it = installed.find(id);
      const auto is_installed = installed_it != installed.end();

      entry["installed"] = is_installed;
      entry["installed_version"] = is_installed ? nlohmann::json(installed_it->second.version) : nlohmann::json(nullptr);
      entry["platform_supported"] = platform_supported(entry.value("platforms", nlohmann::json::array()));

      const auto status = entry.value("status", "listed");
      entry["installable"] = !is_installed && status == "listed" && entry.value("platform_supported", false);
    }

    return result;
  }

  bool
  set_plugin_enabled(std::string_view id, bool enabled, std::string &error) {
    std::lock_guard lock(config_write_mutex);

    if (!find_plugin(id)) {
      error = "unknown plugin";
      return false;
    }

    auto state_path = config_plugin_state_path(id);
    if (!state_path) {
      error = "Sunshine config directory is not available";
      return false;
    }

    std::error_code ec;
    fs::create_directories(state_path->parent_path(), ec);
    if (ec) {
      error = "could not create plugin state directory: " + ec.message();
      return false;
    }

    try {
      std::ofstream file(*state_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) {
        error = "could not open plugin state file";
        return false;
      }

      file << nlohmann::json { { "enabled", enabled } }.dump(2);
      return true;
    }
    catch (const std::exception &err) {
      error = err.what();
      return false;
    }
  }

  bool
  save_plugin_config(std::string_view id, const nlohmann::json &config, std::string &error) {
    std::lock_guard lock(config_write_mutex);

    if (!config.is_object()) {
      error = "plugin config must be a JSON object";
      return false;
    }
    if (!find_plugin(id)) {
      error = "unknown plugin";
      return false;
    }

    auto config_path = config_plugin_config_path(id);
    if (!config_path) {
      error = "Sunshine config directory is not available";
      return false;
    }

    std::error_code ec;
    fs::create_directories(config_path->parent_path(), ec);
    if (ec) {
      error = "could not create plugin config directory: " + ec.message();
      return false;
    }

    try {
      std::ofstream file(*config_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) {
        error = "could not open plugin config file";
        return false;
      }

      file << config.dump(2);
      return true;
    }
    catch (const std::exception &err) {
      error = err.what();
      return false;
    }
  }

  bool
  run_plugin_action(std::string_view id, std::string_view action_id, nlohmann::json &result, std::string &error) {
    auto manifest = find_plugin(id);
    if (!manifest) {
      error = "unknown plugin";
      return false;
    }
    if (!manifest->enabled) {
      error = "plugin is disabled";
      return false;
    }
    if (!platform_supported(*manifest)) {
      error = "plugin is not supported on this platform";
      return false;
    }

    const plugin_action_t *action = nullptr;
    for (const auto &candidate : manifest->actions) {
      if (candidate.id == action_id) {
        action = &candidate;
        break;
      }
    }
    if (!action) {
      error = "unknown plugin action";
      return false;
    }

    if (!entry_exists(*manifest)) {
      error = "plugin executable is missing";
      return false;
    }

    nlohmann::json payload {
      {"manual", true},
      {"action", {
        {"id", action->id},
        {"title", action->title},
        {"event", action->event},
        {"danger", action->danger},
      }},
    };
    payload = build_payload(action->event, payload);
    add_plugin_payload(payload, *manifest);

    auto payload_path = write_payload_file(action->event, payload);
    if (!payload_path) {
      error = "could not write plugin payload";
      return false;
    }

    auto cleanup_payload = util::fail_guard([&]() {
      std::error_code ec;
      fs::remove(*payload_path, ec);
    });

    result = invoke_plugin(*manifest, action->event, *payload_path, "manual"sv, action->id);
    result["action_id"] = action->id;
    return true;
  }

}  // namespace plugin
