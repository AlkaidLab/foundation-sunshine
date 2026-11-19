/**
 * @file src/client_interface.cpp
 * @brief Implementation of the specialised client facts and play history.
 *
 * HTTP cache validators and missing-art responses follow fs-rfc9110-http-20261110
 * (RFC 9110 sections 8.8.3, 13.1.2 and 15.5.5). Application settings are read
 * from the existing Boost.PropertyTree JSON parser, whose representation limits
 * are documented by fs-boost-property-tree-20261110.
 */
#include "client_interface.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <unordered_set>

#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

#include "config.h"
#include "display_device/display_device.h"
#include "process.h"
#include "system_info.h"

namespace platf {
  std::filesystem::path
  appdata();
}

namespace client_interface {
  namespace {
    using json = nlohmann::json;

    std::int64_t
    now_epoch_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
    }

    json
    read_json(const std::filesystem::path &path) {
      std::ifstream input { path };
      if (!input) return json::object();
      try {
        json value;
        input >> value;
        return value.is_object() ? value : json::object();
      }
      catch (...) {
        return json::object();
      }
    }

    bool
    write_json(const std::filesystem::path &path, const json &value) {
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) return false;

      const auto temporary = path.string() + ".tmp";
      {
        std::ofstream output { temporary, std::ios::trunc };
        if (!output) return false;
        output << value.dump(2) << '\n';
        if (!output) return false;
      }

      std::filesystem::rename(temporary, path, error);
      if (!error) return true;

#ifdef _WIN32
      std::filesystem::remove(path, error);
      if (error) return false;
      error.clear();
      std::filesystem::rename(temporary, path, error);
#endif
      return !error;
    }

    // The play facts file is read, changed and written by the thread that starts a session, the one that ends it and the readers of an application list: one at a time.
    std::mutex &
    facts_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::int64_t
    integer(const json &value, const char *key) {
      const auto found = value.find(key);
      if (found == value.end() || !found->is_number_integer()) return 0;
      return found->get<std::int64_t>();
    }
  }  // namespace

  namespace {
    host_profile_t
    probe_host_profile() {
      // fs-linux-os-release-20261110 documents the /etc/os-release precedence
      // used by the existing system_info backend; ds5-ms-idxgiadapter-enumoutputs-20261105
      // records the Windows display enumeration facility. The cached inventory
      // keeps this optional profile off the request hot path.
      const auto snapshot = system_info::sample(config::video.adapter_name, false);
      host_profile_t profile;
#ifdef _WIN32
      profile.platform = "windows";
#elif defined(__APPLE__)
      profile.platform = "macos";
#else
      profile.platform = "linux";
#endif
      profile.os_version = snapshot.os.name;
      if (!snapshot.os.version.empty()) {
        if (!profile.os_version.empty()) profile.os_version.push_back(' ');
        profile.os_version += snapshot.os.version;
      }
      if (!snapshot.gpus.empty()) profile.gpu = snapshot.gpus.front().name;
      profile.host_icon = config::nvhttp.fs_host_icon;

      std::unordered_set<std::string> active_devices;
      for (const auto &[id, device] : display_device::enum_available_devices()) {
        if (device.device_state == display_device::device_state_e::active ||
            device.device_state == display_device::device_state_e::primary) {
          active_devices.insert(id);
        }
      }
      const auto modes = display_device::get_current_display_modes(active_devices);
      std::set<std::string> formatted_modes;
      for (const auto &[_, mode] : modes) {
        if (mode.refresh_rate.denominator == 0) continue;
        formatted_modes.insert(
          std::to_string(mode.resolution.width) + "x" +
          std::to_string(mode.resolution.height) + "@" +
          std::to_string(mode.refresh_rate.numerator / mode.refresh_rate.denominator));
      }
      for (const auto &mode : formatted_modes) {
        if (!profile.display_modes.empty()) profile.display_modes.push_back(',');
        profile.display_modes += mode;
      }
      return profile;
    }
  }  // namespace

  host_profile_t
  host_profile() {
    // The system inventory is cached by system_info, but the display enumeration is a Windows call (QueryDisplayConfig) and serverinfo is polled by every client every few seconds:
    // the profile is read at most once in 30 s.
    static std::mutex mutex;
    static host_profile_t cached;
    static std::chrono::steady_clock::time_point at {};
    std::lock_guard lock { mutex };
    const auto now = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point {} && now - at < std::chrono::seconds { 30 }) return cached;
    cached = probe_host_profile();
    at = now;
    return cached;
  }

  void
  append_serverinfo(boost::property_tree::ptree &tree, bool paired) {
    tree.put("root.FsCapabilityVersion", capability_version);
    // fs-poster-variants-20261112: clients opt into the preview representation
    // only after this additive capability is advertised.
    std::string features = "profile,appfacts,art,poster_variants";
#ifdef _WIN32
    for (const auto &app : proc::proc.get_apps()) {
      if (app.fs_facts.is_bigscreen_entry) {
        features += ",bigscreen";
        break;
      }
    }
#endif
    tree.put("root.FsFeatures", features);
    // The client reads this element. 1 opts in; anything else is left alone.
    tree.put("root.poster_variants", 1);
    if (!paired) return;
    const auto profile = host_profile();
    if (!profile.platform.empty()) tree.put("root.FsPlatform", profile.platform);
    if (!profile.os_version.empty()) tree.put("root.FsOsVersion", profile.os_version);
    if (!profile.gpu.empty()) tree.put("root.FsGpu", profile.gpu);
    if (!profile.display_modes.empty()) tree.put("root.FsDisplayModes", profile.display_modes);
    if (!profile.host_icon.empty()) tree.put("root.FsHostIcon", profile.host_icon);
  }

  app_facts_t
  make_app_facts(std::string app_id,
    std::string command,
    bool opens_big_screen,
    std::string poster_path,
    const boost::property_tree::ptree &app_node) {
    const auto kind = app_node.get_optional<std::string>("fs-kind");
    const auto is_default = app_node.get_optional<bool>("fs-default");
    const auto source = app_node.get_optional<std::string>("fs-source");
    const auto hero_path = app_node.get_optional<std::string>("hero");
    const auto logo_path = app_node.get_optional<std::string>("logo");
    const auto icon_path = app_node.get_optional<std::string>("icon");
    app_facts_t facts;
    facts.is_bigscreen_entry = opens_big_screen;
    facts.kind = infer_kind(opens_big_screen, !command.empty(), command);
    if (kind) {
      if (const auto parsed = parse_kind(*kind)) {
        facts.kind = *parsed;
        facts.kind_overridden = true;
      }
    }
    facts.is_default = is_default.value_or(false);
    facts.source = source && !source->empty() ? *source : infer_source(command);
    facts.hero_path = hero_path.value_or("");
    facts.logo_path = logo_path.value_or("");
    facts.icon_path = icon_path.value_or("");
    facts.art_version = make_art_version(app_id, poster_path, facts.hero_path, facts.logo_path, facts.icon_path);
    if (const auto record = read_play_record(play_facts_path(), app_id)) {
      facts.last_played = record->last_played;
      facts.play_seconds = record->play_seconds;
    }
    return facts;
  }

  std::filesystem::path
  play_facts_path() {
    return platf::appdata() / "fs-app-facts.json";
  }

  std::string
  play_facts_tag() {
    std::error_code error;
    const auto path = play_facts_path();
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    const auto written = std::filesystem::last_write_time(path, error).time_since_epoch().count();
    if (error) return {};
    return std::to_string(size) + ':' + std::to_string(written);
  }

  std::unordered_map<std::string, play_record_t>
  read_all_play_records(const std::filesystem::path &path) {
    std::unordered_map<std::string, play_record_t> records;
    std::lock_guard lock { facts_mutex() };
    const auto root = read_json(path);
    const auto apps = root.find("apps");
    if (apps == root.end() || !apps->is_object()) return records;
    for (const auto &[id, entry] : apps->items()) {
      if (!entry.is_object()) continue;
      records.emplace(id,
        play_record_t {
          .last_played = integer(entry, "last_played"),
          .play_seconds = std::max<std::int64_t>(0, integer(entry, "play_seconds")),
          .started = std::max<std::int64_t>(0, integer(entry, "started")),
        });
    }
    return records;
  }

  std::optional<play_record_t>
  read_play_record(const std::filesystem::path &path, std::string_view app_id) {
    const auto root = read_json(path);
    const auto apps = root.find("apps");
    if (apps == root.end() || !apps->is_object()) return std::nullopt;
    const auto found = apps->find(std::string { app_id });
    if (found == apps->end() || !found->is_object()) return std::nullopt;
    return play_record_t {
      .last_played = integer(*found, "last_played"),
      .play_seconds = std::max<std::int64_t>(0, integer(*found, "play_seconds")),
      .started = std::max<std::int64_t>(0, integer(*found, "started")),
    };
  }

  bool
  write_play_record(const std::filesystem::path &path,
    std::string_view app_id,
    std::int64_t now,
    std::int64_t elapsed_seconds) {
    if (app_id.empty() || now <= 0) return false;
    auto root = read_json(path);
    if (!root.contains("version")) root["version"] = 1;
    auto &apps = root["apps"];
    if (!apps.is_object()) apps = json::object();

    auto &entry = apps[std::string { app_id }];
    const auto previous = read_play_record(path, app_id).value_or(play_record_t {});
    entry["last_played"] = now;
    entry["play_seconds"] = previous.play_seconds + std::max<std::int64_t>(0, elapsed_seconds);
    entry["started"] = 0;
    return write_json(path, root);
  }

  bool
  record_session_start(std::string_view app_id, std::int64_t now) {
    if (app_id.empty() || now <= 0) return false;
    std::lock_guard lock { facts_mutex() };
    const auto path = play_facts_path();
    auto root = read_json(path);
    if (!root.contains("version")) root["version"] = 1;
    auto &apps = root["apps"];
    if (!apps.is_object()) apps = json::object();
    auto &entry = apps[std::string { app_id }];
    if (!entry.is_object()) entry = json::object();
    entry["started"] = now;
    return write_json(path, root);
  }

  bool
  record_session_start(std::string_view app_id) {
    return record_session_start(app_id, now_epoch_seconds());
  }

  bool
  record_session_end(std::string_view app_id, std::int64_t now, std::int64_t elapsed_seconds) {
    if (app_id.empty() || now <= 0) return false;
    std::lock_guard lock { facts_mutex() };
    const auto path = play_facts_path();
    const auto record = read_play_record(path, app_id).value_or(play_record_t {});
    return write_play_record(path, app_id, now, measured_session_seconds(record.started, now, elapsed_seconds));
  }

  bool
  record_session_end(std::string_view app_id, std::int64_t elapsed_seconds) {
    return record_session_end(app_id, now_epoch_seconds(), elapsed_seconds);
  }

  bool
  record_session_end(std::string_view app_id, std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - started)
                           .count();
    return record_session_end(app_id, elapsed);
  }
}  // namespace client_interface
