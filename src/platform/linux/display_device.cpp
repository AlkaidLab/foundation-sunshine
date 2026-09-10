// Linux implementation of the display_device layer.
//
// State reads and writes are mediated by the compositor through
// kscreen-doctor (KDE Plasma; any compositor the tool can talk to). When the
// tool is unavailable (other desktop environments, no session environment),
// every getter degrades to empty results and every setter to a logged
// no-op, which keeps streams working with the historical stub behavior.
//
// The apply/revert flow mirrors the Windows backend in
// src/platform/windows/display_device/settings.cpp, minus Windows-specific
// pieces (CCD topology metadata, duplicated-device groups, ICC color
// profiles, audio session extension). Topology groups are one device each;
// VDD topology stays under the control of the session's VDD stage, exactly
// as it does on Windows.

// local includes
#include "src/display_device/settings.h"
#include "src/display_device/to_string.h"
#include "src/display_device/vdd_utils.h"
#include "src/globals.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/display_device/session.h"
#include "src/logging.h"
#include "src/utility.h"

namespace display_device {

  struct settings_t::persistent_data_t {
    active_topology_t initial_topology; /**< Topology before our first modification. */
    active_topology_t modified_topology; /**< Topology we applied for the session. */
    std::string original_primary_display; /**< Original primary display. Empty if we didn't modify it. */
    device_display_mode_map_t original_modes; /**< Original display modes. Empty if we didn't modify them. */
    hdr_state_map_t original_hdr_states; /**< Original HDR states. Empty if we didn't modify them. */

    [[nodiscard]] bool
    contains_modifications() const {
      return !is_topology_the_same(initial_topology, modified_topology) ||
             !original_primary_display.empty() ||
             !original_modes.empty() ||
             !original_hdr_states.empty();
    }

    friend void
    to_json(nlohmann::json &json, const persistent_data_t &data) {
      json = {
        { "topology", {
                        { "initial", data.initial_topology },
                        { "modified", data.modified_topology },
                      } },
        { "original_primary_display", data.original_primary_display },
        { "original_modes", data.original_modes },
        { "original_hdr_states", data.original_hdr_states },
      };
    }

    friend void
    from_json(const nlohmann::json &json, persistent_data_t &data) {
      json.at("topology").at("initial").get_to(data.initial_topology);
      json.at("topology").at("modified").get_to(data.modified_topology);
      json.at("original_primary_display").get_to(data.original_primary_display);
      json.at("original_modes").get_to(data.original_modes);
      json.at("original_hdr_states").get_to(data.original_hdr_states);
    }
  };

  namespace {

    struct kscreen_mode_t {
      std::string id;
      unsigned int width = 0;
      unsigned int height = 0;
      double refresh = 0.0;
      std::string refresh_str;  // as printed by kscreen-doctor, e.g. "60.00"
      bool current = false;
      bool preferred = false;
    };

    struct kscreen_output_t {
      int index = 0;
      std::string name;
      bool enabled = false;
      bool connected = false;
      int priority = 0;
      int replication_source = 0;
      std::vector<kscreen_mode_t> modes;
      std::string hdr_state_str;  // "enabled" / "disabled" / "incapable"; empty = not reported
    };

    std::string
    strip_ansi(const std::string &text) {
      static const std::regex ansi_re { "\x1b\\[[0-9;]*[A-Za-z]" };
      return std::regex_replace(text, ansi_re, "");
    }

    struct exec_result_t {
      int exit_code = -1;
      std::string output;
    };

    exec_result_t
    run_kscreen(const std::string &args) {
      exec_result_t result;
      std::string output;
      if (FILE *pipe = popen(("kscreen-doctor " + args + " 2>&1").c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) {
          output += buf;
        }
        const int status = pclose(pipe);
        if (WIFEXITED(status)) {
          result.exit_code = WEXITSTATUS(status);
        }
      }
      result.output = strip_ansi(output);
      return result;
    }

    /**
     * @brief Run a kscreen-doctor change command and verify that it took
     *        effect, retrying a bounded number of times (the compositor
     *        applies changes asynchronously).
     */
    template<typename ConfirmedFn>
    bool
    kscreen_change(const std::string &args, ConfirmedFn &&confirmed) {
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (confirmed()) {
          return true;
        }

        const auto result = run_kscreen(args);
        BOOST_LOG(debug) << "kscreen-doctor [" << args << "] exit=" << result.exit_code
                         << (result.output.empty() ? "" : " output: " + result.output);

        if (result.exit_code == 0 && confirmed()) {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
      }
      return confirmed();
    }

    std::vector<kscreen_output_t>
    query_outputs() {
      const auto result = run_kscreen("-o");
      if (result.exit_code != 0) {
        static std::atomic<bool> failure_logged { false };
        if (!failure_logged.exchange(true)) {
          BOOST_LOG(info) << "kscreen-doctor unavailable (exit " << result.exit_code
                          << "); compositor display management is disabled" ;
        }
        return {};
      }

      std::vector<kscreen_output_t> outputs;

      static const std::regex output_re { R"(^Output:\s+(\d+)\s+(\S+)\s+\S+)" };
      static const std::regex modes_re { R"((\d+):(\d+)x(\d+)@([0-9.]+)(\*)?(!)?)" };
      static const std::regex priority_re { R"(^priority\s+(\d+))" };

      kscreen_output_t *current = nullptr;
      std::istringstream stream { result.output };
      std::string line;
      while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, output_re)) {
          kscreen_output_t output;
          output.index = std::atoi(match[1].str().c_str());
          output.name = match[2].str();
          outputs.push_back(std::move(output));
          current = &outputs.back();
          continue;
        }
        if (!current) {
          continue;
        }

        const auto trimmed = [](std::string value) {
          const auto first = value.find_first_not_of(" \t");
          if (first == std::string::npos) {
            return std::string {};
          }
          const auto last = value.find_last_not_of(" \t\r");
          return value.substr(first, last - first + 1);
        }(line);

        if (trimmed == "enabled") {
          current->enabled = true;
        }
        else if (trimmed == "connected") {
          current->connected = true;
        }
        else if (trimmed.rfind("HDR:", 0) == 0) {
          current->hdr_state_str = trimmed.substr(4);
          // trim again for the value itself
          const auto first = current->hdr_state_str.find_first_not_of(" \t");
          current->hdr_state_str = first == std::string::npos ?
                                     std::string {} :
                                     current->hdr_state_str.substr(current->hdr_state_str.find_last_not_of(" \t\r") - first + 1);
        }
        else if (std::regex_search(trimmed, match, priority_re)) {
          current->priority = std::atoi(match[1].str().c_str());
        }
        else if (trimmed.rfind("replication source:", 0) == 0) {
          current->replication_source = std::atoi(trimmed.substr(19).c_str());
        }
        else if (trimmed.rfind("Modes:", 0) == 0) {
          auto begin = std::sregex_iterator { trimmed.begin(), trimmed.end(), modes_re };
          const auto end = std::sregex_iterator {};
          for (auto it = begin; it != end; ++it) {
            kscreen_mode_t mode;
            mode.id = (*it)[1].str();
            mode.width = static_cast<unsigned int>(std::atoi((*it)[2].str().c_str()));
            mode.height = static_cast<unsigned int>(std::atoi((*it)[3].str().c_str()));
            mode.refresh_str = (*it)[4].str();
            mode.refresh = std::atof(mode.refresh_str.c_str());
            mode.current = (*it)[5].matched;
            mode.preferred = (*it)[6].matched;
            current->modes.push_back(std::move(mode));
          }
        }
      }

      return outputs;
    }

    const kscreen_output_t *
    find_output(const std::vector<kscreen_output_t> &outputs, const std::string &name) {
      const auto it = std::find_if(outputs.begin(), outputs.end(), [&name](const auto &output) {
        return output.name == name;
      });
      return it == outputs.end() ? nullptr : &*it;
    }

    hdr_state_e
    parse_hdr_state(const std::string &state_str) {
      if (state_str == "enabled") {
        return hdr_state_e::enabled;
      }
      if (state_str == "disabled") {
        return hdr_state_e::disabled;
      }
      return hdr_state_e::unknown;
    }

    /**
     * @brief True when the connector is (or was) the virtual display rather
     *        than a physical one: it is live, or it is neither enumerated nor
     *        among the physicals we forced off (a destroyed VDD leaves its
     *        connector disconnected in sysfs).
     */
    bool
    is_vdd_connector(const std::string &device_id) {
      if (device_id.empty()) {
        return false;
      }
      if (device_id == vdd_utils::live_virtual_display_connector()) {
        return true;
      }
      const auto devices = enum_available_devices();
      if (devices.count(device_id)) {
        return false;
      }
      const auto offlined = vdd_utils::offlined_physical_connectors();
      return std::find(offlined.begin(), offlined.end(), device_id) == offlined.end();
    }

    bool
    topology_contains_vdd(const active_topology_t &topology) {
      for (const auto &group : topology) {
        for (const auto &device_id : group) {
          if (is_vdd_connector(device_id)) {
            return true;
          }
        }
      }
      return false;
    }

    std::unordered_set<std::string>
    topology_device_ids(const active_topology_t &topology) {
      std::unordered_set<std::string> ids;
      for (const auto &group : topology) {
        ids.insert(group.begin(), group.end());
      }
      return ids;
    }

    active_topology_t
    canonical_topology(const active_topology_t &topology) {
      active_topology_t groups;
      for (const auto &group : topology) {
        if (group.empty()) {
          continue;
        }
        auto sorted { group };
        std::sort(sorted.begin(), sorted.end());
        groups.push_back(std::move(sorted));
      }
      std::sort(groups.begin(), groups.end());
      return groups;
    }

    active_topology_t
    determine_target_topology(parsed_config_t::device_prep_e prep, const std::string &device_id, const active_topology_t &current) {
      active_topology_t target { current };

      const auto ensure_present = [&target, &device_id]() {
        if (device_id.empty() || topology_device_ids(target).count(device_id)) {
          return;
        }
        target.push_back({ device_id });
      };

      switch (prep) {
        case parsed_config_t::device_prep_e::no_operation:
          break;
        case parsed_config_t::device_prep_e::ensure_active:
          ensure_present();
          break;
        case parsed_config_t::device_prep_e::ensure_primary:
          ensure_present();
          break;
        case parsed_config_t::device_prep_e::ensure_only_display:
          if (device_id.empty()) {
            BOOST_LOG(warning) << "ensure_only_display requested without a resolvable device id; keeping current topology";
            break;
          }
          target = { { device_id } };
          break;
        case parsed_config_t::device_prep_e::ensure_secondary:
          ensure_present();
          break;
      }

      return target;
    }

    std::string
    find_primary_in_topology(const active_topology_t &topology) {
      for (const auto &group : topology) {
        for (const auto &device_id : group) {
          if (is_primary_device(device_id)) {
            return device_id;
          }
        }
      }
      return {};
    }

    bool
    wait_for_topology(const active_topology_t &expected, std::chrono::milliseconds budget) {
      const auto deadline = std::chrono::steady_clock::now() + budget;
      while (std::chrono::steady_clock::now() < deadline) {
        if (is_topology_the_same(get_current_topology(), expected)) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
      }
      BOOST_LOG(warning) << "Timed out waiting for the topology to stabilize";
      return false;
    }

    bool
    save_settings(const std::filesystem::path &filepath, const settings_t::persistent_data_t &data) {
      if (filepath.empty()) {
        BOOST_LOG(warning) << "No filename was specified for persistent display device configuration.";
        return true;
      }

      try {
        std::ofstream file(filepath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
          BOOST_LOG(error) << "Failed to open persistent display settings for writing: " << filepath;
          return false;
        }
        nlohmann::json json_data = data;
        file << std::setw(4) << json_data << std::endl;
        if (!file) {
          BOOST_LOG(error) << "Failed to write persistent display settings: " << filepath;
          return false;
        }
        return true;
      }
      catch (const std::exception &err) {
        BOOST_LOG(error) << "Failed to save display settings: " << err.what();
      }

      return false;
    }

    std::unique_ptr<settings_t::persistent_data_t>
    load_settings(const std::filesystem::path &filepath) {
      try {
        if (!filepath.empty() && std::filesystem::exists(filepath)) {
          std::ifstream file(filepath);
          return std::make_unique<settings_t::persistent_data_t>(nlohmann::json::parse(file));
        }
      }
      catch (const std::exception &err) {
        BOOST_LOG(error) << "Failed to load saved display settings: " << err.what();
      }

      return nullptr;
    }

    void
    remove_file(const std::filesystem::path &filepath) {
      try {
        if (!filepath.empty()) {
          std::filesystem::remove(filepath);
        }
      }
      catch (const std::exception &err) {
        BOOST_LOG(error) << "Failed to remove " << filepath << ". Error: " << err.what();
      }
    }

    template<typename MapT>
    void
    filter_stale_devices(MapT &map, const std::unordered_set<std::string> &valid_ids, const char *label) {
      for (auto it = map.begin(); it != map.end();) {
        if (!valid_ids.count(it->first) || is_vdd_connector(it->first)) {
          BOOST_LOG(debug) << "Removing stale/vdd device from " << label << ": " << it->first;
          it = map.erase(it);
        }
        else {
          ++it;
        }
      }
    }

    template<typename MapT>
    void
    remove_vdd_entries(MapT &map, const char *label) {
      for (auto it = map.begin(); it != map.end();) {
        if (is_vdd_connector(it->first)) {
          BOOST_LOG(debug) << "Removed VDD entry from " << label << ": " << it->first;
          it = map.erase(it);
        }
        else {
          ++it;
        }
      }
    }

    void
    remove_vdd_from_topology(active_topology_t &topology) {
      for (auto &group : topology) {
        group.erase(
          std::remove_if(group.begin(), group.end(), [](const std::string &id) { return is_vdd_connector(id); }),
          group.end());
      }
      topology.erase(
        std::remove_if(topology.begin(), topology.end(), [](const std::vector<std::string> &group) { return group.empty(); }),
        topology.end());
    }

  }  // namespace

  device_info_map_t
  enum_available_devices() {
    // Enumerate live DRM connectors from sysfs. The session flow matches
    // displays by connector name ("DP-1", "eDP-1", ...), and the headless /
    // stale-VDD checks rely on physical connectors being visible here.
    namespace fs = std::filesystem;

    device_info_map_t devices;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator { "/sys/class/drm", ec }) {
      const auto name = entry.path().filename().string();
      if (name.rfind("card", 0) != 0) {
        continue;
      }
      const auto card_end = name.find('-');
      if (card_end == std::string::npos) {
        continue;
      }
      const std::string connector = name.substr(card_end + 1);

      std::ifstream status_file { entry.path() / "status" };
      std::string status;
      std::getline(status_file, status);

      // Physical connectors powered down for an exclusive virtual display
      // session read as plain "disconnected" in sysfs (NVIDIA does not
      // distinguish), so the backend is consulted for them: they must stay
      // visible (as inactive) - otherwise the session teardown's headless
      // guard sees only the virtual display and skips restoring the screen.
      const bool connected = status == "connected";
      bool forced_off = status == "off";
      if (!connected && !forced_off) {
        const auto offlined = vdd_utils::offlined_physical_connectors();
        forced_off = std::find(offlined.begin(), offlined.end(), connector) != offlined.end();
      }
      if (!connected && !forced_off) {
        continue;
      }

      device_info_t info;
      info.display_name = connector;
      info.friendly_name = connector;
      info.device_state = connected ? device_state_e::active : device_state_e::inactive;
      info.hdr_state = hdr_state_e::unknown;
      devices.emplace(connector, std::move(info));
    }

    if (devices.empty()) {
      BOOST_LOG(debug) << "display enumeration found no live connectors";
    }
    else {
      std::string names;
      for (const auto &[id, info] : devices) {
        names += id + (info.device_state == device_state_e::active ? "(active) " : "(inactive) ");
      }
      BOOST_LOG(debug) << "display enumeration: " << names;
    }

    return devices;
  }

  std::string
  get_display_name(const std::string &value) {
    // Not implemented, but just passthrough the value
    return value;
  }

  std::string
  get_display_friendly_name(const std::string &device_id) {
    if (device_id == vdd_utils::live_virtual_display_connector()) {
      return std::string { ZAKO_NAME };
    }
    return device_id;
  }

  device_display_mode_map_t
  get_current_display_modes(const std::unordered_set<std::string> &device_ids) {
    device_display_mode_map_t modes;
    const auto outputs = query_outputs();

    for (const auto &output : outputs) {
      if (!output.enabled) {
        continue;
      }
      if (!device_ids.empty() && !device_ids.count(output.name)) {
        continue;
      }

      const auto current_mode = std::find_if(output.modes.begin(), output.modes.end(), [](const auto &mode) {
        return mode.current;
      });
      if (current_mode == output.modes.end()) {
        continue;
      }

      const auto refresh_ratio = [&mode = *current_mode]() -> refresh_rate_t {
        // Reproduce the printed refresh rate as an exact fraction (2 decimals).
        const auto denominator = 100u;
        const auto numerator = static_cast<unsigned int>(std::llround(mode.refresh * denominator));
        if (numerator == 0) {
          return { 0, 1 };
        }
        const auto gcd = std::gcd(numerator, denominator);
        return { numerator / gcd, denominator / gcd };
      }();

      modes.emplace(output.name, display_mode_t { { current_mode->width, current_mode->height }, refresh_ratio });
    }

    return modes;
  }

  bool
  set_display_modes(const device_display_mode_map_t &modes) {
    if (modes.empty()) {
      return true;
    }

    bool all_ok = true;
    for (const auto &[device_id, mode] : modes) {
      const auto outputs = query_outputs();
      const auto *output = find_output(outputs, device_id);
      if (!output) {
        BOOST_LOG(error) << "Cannot set display mode for unknown output: " << device_id;
        all_ok = false;
        continue;
      }

      const double target_refresh = static_cast<double>(mode.refresh_rate.numerator) /
                                    static_cast<double>(mode.refresh_rate.denominator);

      const auto current_matches = [&]() {
        const auto fresh = query_outputs();
        const auto *fresh_output = find_output(fresh, device_id);
        if (!fresh_output) {
          return false;
        }
        const auto current_mode = std::find_if(fresh_output->modes.begin(), fresh_output->modes.end(), [](const auto &m) {
          return m.current;
        });
        return current_mode != fresh_output->modes.end() &&
               current_mode->width == mode.resolution.width &&
               current_mode->height == mode.resolution.height &&
               std::fabs(current_mode->refresh - target_refresh) <= 0.051;
      };

      if (current_matches()) {
        continue;
      }

      const kscreen_mode_t *best = nullptr;
      for (const auto &candidate : output->modes) {
        if (candidate.width != mode.resolution.width || candidate.height != mode.resolution.height) {
          continue;
        }
        if (std::fabs(candidate.refresh - target_refresh) > 0.051) {
          continue;
        }
        best = &candidate;
        break;
      }

      if (!best) {
        std::string available;
        for (const auto &candidate : output->modes) {
          available += candidate.id + ":" + std::to_string(candidate.width) + "x" +
                       std::to_string(candidate.height) + "@" + candidate.refresh_str + " ";
        }
        BOOST_LOG(error) << "Output " << device_id << " has no mode "
                         << mode.resolution.width << "x" << mode.resolution.height << "@"
                         << target_refresh << " (available: " << available << ")";
        all_ok = false;
        continue;
      }

      const std::string base_args = "output." + device_id + ".mode.";
      // kscreen-doctor rejects a decimal refresh rate in the WxH@refresh
      // form, so integral refreshes go by rate and fractional ones (e.g.
      // 59.94) only by their mode id.
      std::vector<std::string> attempts;
      if (std::fabs(best->refresh - std::round(best->refresh)) < 0.001) {
        attempts.push_back(std::to_string(best->width) + "x" + std::to_string(best->height) + "@" +
                           std::to_string(static_cast<long long>(std::llround(best->refresh))));
      }
      attempts.push_back(best->id);

      bool applied = false;
      for (const auto &attempt : attempts) {
        BOOST_LOG(info) << "Changing display mode: " << base_args << attempt;
        if (kscreen_change(base_args + attempt, current_matches)) {
          applied = true;
          break;
        }
      }
      if (!applied) {
        BOOST_LOG(error) << "Failed to apply display mode for output " << device_id;
        all_ok = false;
      }
    }

    return all_ok;
  }

  bool
  is_primary_device(const std::string &device_id) {
    const auto outputs = query_outputs();
    const auto *output = find_output(outputs, device_id);
    return output && output->enabled && output->priority == 1;
  }

  bool
  set_as_primary_device(const std::string &device_id) {
    if (device_id.empty()) {
      return false;
    }

    const auto already_primary = [&]() {
      return is_primary_device(device_id);
    };

    if (already_primary()) {
      return true;
    }

    BOOST_LOG(info) << "Setting primary display: " << device_id;
    return kscreen_change("output." + device_id + ".priority.1", already_primary);
  }

  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &device_ids) {
    hdr_state_map_t states;
    const auto outputs = query_outputs();

    for (const auto &output : outputs) {
      if (!output.enabled || output.hdr_state_str.empty()) {
        continue;
      }
      if (!device_ids.empty() && !device_ids.count(output.name)) {
        continue;
      }
      states.emplace(output.name, parse_hdr_state(output.hdr_state_str));
    }

    return states;
  }

  bool
  set_hdr_states(const hdr_state_map_t &states) {
    if (states.empty()) {
      return true;
    }

    bool all_ok = true;
    for (const auto &[device_id, state] : states) {
      if (state == hdr_state_e::unknown) {
        continue;
      }

      const auto state_matches = [&]() {
        const auto fresh = get_current_hdr_states({ device_id });
        const auto it = fresh.find(device_id);
        return it != fresh.end() && it->second == state;
      };

      if (state_matches()) {
        continue;
      }

      const std::string args = "output." + device_id + (state == hdr_state_e::enabled ? ".hdr.enable" : ".hdr.disable");
      BOOST_LOG(info) << "Changing HDR state: " << args;
      if (!kscreen_change(args, state_matches)) {
        BOOST_LOG(error) << "Failed to set HDR state for output " << device_id;
        all_ok = false;
      }
    }

    return all_ok;
  }

  active_topology_t
  get_current_topology() {
    active_topology_t topology;
    const auto outputs = query_outputs();

    for (const auto &output : outputs) {
      if (output.enabled) {
        topology.push_back({ output.name });
      }
    }

    // Duplicated groups (replication source) are not representable in the
    // topology we can set back; extended-only grouping is the Linux norm.
    return topology;
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    if (topology.empty()) {
      return false;
    }

    std::unordered_set<std::string> seen;
    for (const auto &group : topology) {
      if (group.empty()) {
        return false;
      }
      for (const auto &device_id : group) {
        if (device_id.empty() || !seen.insert(device_id).second) {
          return false;
        }
      }
    }

    return true;
  }

  bool
  is_topology_the_same(const active_topology_t &topology_a, const active_topology_t &topology_b) {
    return canonical_topology(topology_a) == canonical_topology(topology_b);
  }

  bool
  set_topology(const active_topology_t &new_topology) {
    if (!is_topology_valid(new_topology)) {
      BOOST_LOG(error) << "Refusing to set an invalid topology: " << to_string(new_topology);
      return false;
    }

    for (const auto &group : new_topology) {
      if (group.size() > 1) {
        BOOST_LOG(error) << "Duplicated display groups are not supported on this backend: " << to_string(new_topology);
        return false;
      }
    }

    const auto current = get_current_topology();
    const auto current_ids = topology_device_ids(current);
    const auto target_ids = topology_device_ids(new_topology);

    for (const auto &device_id : target_ids) {
      if (!current_ids.count(device_id)) {
        BOOST_LOG(info) << "Enabling output: " << device_id;
        const auto result = run_kscreen("output." + device_id + ".enable");
        if (result.exit_code != 0) {
          BOOST_LOG(error) << "Failed to enable output " << device_id << ": " << result.output;
          return false;
        }
      }
    }

    for (const auto &device_id : current_ids) {
      if (!target_ids.count(device_id)) {
        BOOST_LOG(info) << "Disabling output: " << device_id;
        const auto result = run_kscreen("output." + device_id + ".disable");
        if (result.exit_code != 0) {
          BOOST_LOG(error) << "Failed to disable output " << device_id << ": " << result.output;
          return false;
        }
      }
    }

    return true;
  }

  struct settings_t::audio_data_t {
    // Audio session extension is a Windows-only concept.
  };

  settings_t::settings_t() = default;

  settings_t::~settings_t() = default;

  bool
  settings_t::is_changing_settings_going_to_fail() const {
    return false;
  }

  void
  settings_t::capture_audio_sink() {
    // Windows-only: extends the audio session across topology changes.
  }

  void
  settings_t::release_audio_sink() {
    // Windows-only.
  }

  settings_t::apply_result_t
  settings_t::apply_config(
    const parsed_config_t &config,
    const rtsp_stream::launch_session_t &,
    const boost::optional<active_topology_t> &pre_saved_initial_topology) {
    BOOST_LOG(info) << "Applying configuration to the display device.";

    const bool is_vdd_mode = config.use_vdd && *config.use_vdd;

    const auto current_topology = get_current_topology();
    if (current_topology.empty()) {
      // Nothing the compositor layer can manage (kscreen-doctor missing or no
      // enabled outputs): keep the historical no-op behavior so streaming
      // does not regress on setups where this backend cannot operate.
      BOOST_LOG(info) << "Compositor display management unavailable; skipping display configuration.";
      return { apply_result_t::result_e::success };
    }

    const std::string device_id = find_one_of_the_available_devices(config.device_id);

    active_topology_t initial_topology = current_topology;
    if (pre_saved_initial_topology && !pre_saved_initial_topology->empty()) {
      initial_topology = *pre_saved_initial_topology;
    }

    // In VDD mode the session's VDD display stage owns topology changes
    // (apply_vdd_prep); we only record the baseline here.
    const active_topology_t modified_topology = is_vdd_mode || config.device_prep == parsed_config_t::device_prep_e::no_operation ?
                                                  current_topology :
                                                  determine_target_topology(config.device_prep, device_id, current_topology);

    persistent_data_t new_settings;
    new_settings.initial_topology = initial_topology;
    new_settings.modified_topology = modified_topology;
    persistent_data_t &current_settings = persistent_data ? *persistent_data : new_settings;
    if (persistent_data) {
      current_settings.modified_topology = modified_topology;
    }

    const bool skip_vdd_only_baseline = is_vdd_mode &&
                                        !persistent_data &&
                                        !pre_saved_initial_topology &&
                                        !initial_topology.empty() &&
                                        topology_device_ids(initial_topology).size() == 1 &&
                                        is_vdd_connector(*topology_device_ids(initial_topology).begin());

    const auto persist_settings = [&]() -> apply_result_t {
      if (current_settings.contains_modifications()) {
        if (!persistent_data) {
          if (skip_vdd_only_baseline) {
            BOOST_LOG(warning) << "Refusing to persist a VDD-only topology as the restore baseline; continuing without display restore data.";
            return { apply_result_t::result_e::success };
          }
          persistent_data = std::make_unique<persistent_data_t>(new_settings);
        }

        if (!save_settings(filepath, *persistent_data)) {
          return { apply_result_t::result_e::file_save_fail };
        }
      }
      else if (persistent_data) {
        if (!revert_settings(revert_reason_e::config_cleanup)) {
          return { apply_result_t::result_e::revert_fail };
        }
      }

      return { apply_result_t::result_e::success };
    };

    auto save_guard = util::fail_guard([&]() {
      persist_settings();  // Ignoring the return value
    });

    // Topology (physical display mode only).
    if (!is_vdd_mode && !is_topology_the_same(current_topology, modified_topology)) {
      BOOST_LOG(info) << "Changing display topology: " << to_string(current_topology) << " -> " << to_string(modified_topology);
      if (!set_topology(modified_topology)) {
        return { apply_result_t::result_e::topology_fail };
      }
      wait_for_topology(modified_topology, std::chrono::milliseconds { 5000 });
    }

    // Primary display (physical display mode only; the VDD session stage
    // handles the primary hints for virtual display preps).
    if (!is_vdd_mode && config.device_prep == parsed_config_t::device_prep_e::ensure_primary) {
      const std::string original_primary = current_settings.original_primary_display.empty() ?
                                             find_primary_in_topology(initial_topology.empty() ? current_topology : initial_topology) :
                                             current_settings.original_primary_display;

      if (!device_id.empty() && !set_as_primary_device(device_id)) {
        return { apply_result_t::result_e::primary_display_fail };
      }

      current_settings.original_primary_display = original_primary;
    }

    // Display modes.
    if (config.resolution || config.refresh_rate) {
      const auto topology_ids = topology_device_ids(current_settings.modified_topology.empty() ? modified_topology : current_settings.modified_topology);

      device_display_mode_map_t original_modes { current_settings.original_modes };
      for (const auto &[id, mode] : get_current_display_modes(topology_ids)) {
        original_modes.try_emplace(id, mode);
      }

      device_display_mode_map_t new_modes { original_modes };
      std::unordered_set<std::string> targets;
      if (!device_id.empty()) {
        targets.insert(device_id);
      }
      else {
        targets = topology_ids;
      }

      for (const auto &target : targets) {
        auto it = new_modes.find(target);
        if (it == new_modes.end()) {
          // Output without a readable current mode (e.g. freshly enabled);
          // build the entry from the request itself.
          it = new_modes.emplace(target,
                 display_mode_t { config.resolution.value_or(resolution_t { 0, 0 }),
                   config.refresh_rate.value_or(refresh_rate_t { 0, 1 }) })
                 .first;
        }
        if (config.resolution) {
          it->second.resolution = *config.resolution;
        }
        if (config.refresh_rate) {
          it->second.refresh_rate = *config.refresh_rate;
        }
      }

      filter_stale_devices(new_modes, topology_ids, "display modes");

      BOOST_LOG(info) << "Changing display modes to: " << to_string(new_modes);
      if (!set_display_modes(new_modes)) {
        if (is_vdd_mode) {
          // The VDD backend already guarantees the session mode through the
          // personalized EDID; a compositor hiccup here must not fail the stream.
          BOOST_LOG(warning) << "Display mode change failed for the virtual display; continuing (mode is guaranteed by the EDID).";
        }
        else {
          return { apply_result_t::result_e::modes_fail };
        }
      }

      current_settings.original_modes = original_modes;
      filter_stale_devices(current_settings.original_modes, topology_ids, "original display modes");
    }
    else if (!current_settings.original_modes.empty()) {
      device_display_mode_map_t filtered_modes { current_settings.original_modes };
      filter_stale_devices(filtered_modes, topology_device_ids(get_current_topology()), "rollback display modes");

      if (!filtered_modes.empty()) {
        BOOST_LOG(info) << "Changing display modes back to: " << to_string(filtered_modes);
        if (!set_display_modes(filtered_modes)) {
          return { apply_result_t::result_e::modes_fail };
        }
      }
      current_settings.original_modes.clear();
    }

    // HDR states.
    if (config.change_hdr_state) {
      const auto topology_ids = topology_device_ids(current_settings.modified_topology.empty() ? modified_topology : current_settings.modified_topology);

      const auto current_hdr_states = get_current_hdr_states(topology_ids);
      if (current_hdr_states.empty() && !is_vdd_mode) {
        return { apply_result_t::result_e::hdr_states_fail };
      }

      hdr_state_map_t original_hdr_states { current_settings.original_hdr_states };
      for (const auto &[id, state] : current_hdr_states) {
        original_hdr_states.try_emplace(id, state);
      }

      hdr_state_map_t new_hdr_states { original_hdr_states };
      const hdr_state_e final_state = *config.change_hdr_state ? hdr_state_e::enabled : hdr_state_e::disabled;

      std::unordered_set<std::string> targets;
      if (!device_id.empty()) {
        targets.insert(device_id);
      }
      else {
        targets = topology_ids;
      }

      for (const auto &target : targets) {
        const auto it = new_hdr_states.find(target);
        if (it == new_hdr_states.end() || it->second == hdr_state_e::unknown) {
          continue;
        }
        it->second = final_state;
      }

      filter_stale_devices(new_hdr_states, topology_ids, "HDR states");

      BOOST_LOG(info) << "Changing HDR states to: " << to_string(new_hdr_states);
      if (!set_hdr_states(new_hdr_states)) {
        if (is_vdd_mode) {
          BOOST_LOG(warning) << "HDR state change failed for the virtual display; continuing (HDR metadata comes from the EDID).";
        }
        else {
          return { apply_result_t::result_e::hdr_states_fail };
        }
      }

      current_settings.original_hdr_states = original_hdr_states;
      filter_stale_devices(current_settings.original_hdr_states, topology_ids, "original HDR states");
    }
    else if (!current_settings.original_hdr_states.empty()) {
      hdr_state_map_t filtered_hdr { current_settings.original_hdr_states };
      filter_stale_devices(filtered_hdr, topology_device_ids(get_current_topology()), "rollback HDR states");

      if (!filtered_hdr.empty()) {
        BOOST_LOG(info) << "Changing HDR states back to: " << to_string(filtered_hdr);
        if (!set_hdr_states(filtered_hdr)) {
          return { apply_result_t::result_e::hdr_states_fail };
        }
      }
      current_settings.original_hdr_states.clear();
    }

    save_guard.disable();
    return persist_settings();
  }

  bool
  settings_t::revert_settings(revert_reason_e reason, bool skip_vdd_destroy) {
    static const char *reason_strs[] = { "stream ended", "topology switch", "config cleanup", "persistence reset" };
    BOOST_LOG(info) << "Reverting display device settings (reason: " << reason_strs[static_cast<int>(reason)] << ")";

    if (!persistent_data) {
      persistent_data = load_settings(filepath);
    }

    if (!persistent_data) {
      return true;
    }

    persistent_data_t &data = *persistent_data;
    bool data_updated { false };
    bool partially_failed { false };

    // Destroy a VDD we created if the session is ending and the user does
    // not keep it around; mirrors the Windows backend's persistence rules.
    const bool vdd_in_initial = topology_contains_vdd(data.initial_topology);
    const bool vdd_in_modified = topology_contains_vdd(data.modified_topology);
    if (!skip_vdd_destroy && !config::video.vdd_keep_enabled && vdd_in_modified && !vdd_in_initial) {
      BOOST_LOG(info) << "Destroying the virtual display created for this session";
      session_t::get().destroy_vdd_monitor();
    }

    remove_vdd_from_topology(data.initial_topology);
    remove_vdd_from_topology(data.modified_topology);
    remove_vdd_entries(data.original_modes, "original modes");
    remove_vdd_entries(data.original_hdr_states, "original HDR states");

    // Re-enable the initial topology first so that modes/HDR/primary for
    // outputs disabled during the session can be restored afterwards.
    if (!data.initial_topology.empty() && !is_topology_the_same(get_current_topology(), data.initial_topology)) {
      BOOST_LOG(info) << "Changing display topology back to: " << to_string(data.initial_topology);
      if (set_topology(data.initial_topology) && wait_for_topology(data.initial_topology, std::chrono::milliseconds { 5000 })) {
        data_updated = true;
      }
      else {
        partially_failed = true;
      }
    }

    const auto current_ids = topology_device_ids(get_current_topology());

    if (!data.original_hdr_states.empty()) {
      hdr_state_map_t filtered_hdr { data.original_hdr_states };
      filter_stale_devices(filtered_hdr, current_ids, "rollback HDR states");

      if (filtered_hdr.empty()) {
        data.original_hdr_states.clear();
        data_updated = true;
      }
      else {
        BOOST_LOG(info) << "Changing HDR states back to: " << to_string(filtered_hdr);
        if (set_hdr_states(filtered_hdr)) {
          data.original_hdr_states.clear();
          data_updated = true;
        }
        else {
          partially_failed = true;
        }
      }
    }

    if (!data.original_modes.empty()) {
      device_display_mode_map_t filtered_modes { data.original_modes };
      filter_stale_devices(filtered_modes, current_ids, "rollback display modes");

      if (filtered_modes.empty()) {
        data.original_modes.clear();
        data_updated = true;
      }
      else {
        BOOST_LOG(info) << "Changing display modes back to: " << to_string(filtered_modes);
        if (set_display_modes(filtered_modes)) {
          data.original_modes.clear();
          data_updated = true;
        }
        else {
          partially_failed = true;
        }
      }
    }

    if (!data.original_primary_display.empty()) {
      BOOST_LOG(info) << "Changing the primary display back to: " << data.original_primary_display;
      if (set_as_primary_device(data.original_primary_display)) {
        data.original_primary_display.clear();
        data_updated = true;
      }
      else {
        partially_failed = true;
      }
    }

    if (partially_failed) {
      if (data_updated) {
        save_settings(filepath, data);  // Best effort; retain remaining restore state for retry.
      }
      BOOST_LOG(error) << "Failed to restore display settings; manual adjustment may be required.";
      return false;
    }

    remove_file(filepath);
    persistent_data.reset();

    BOOST_LOG(info) << "Display device configuration restored.";
    return true;
  }

  void
  settings_t::reset_persistence() {
    BOOST_LOG(info) << "Purging persistent display device data (trying to reset settings one last time).";
    if (persistent_data && !revert_settings(revert_reason_e::persistence_reset)) {
      BOOST_LOG(info) << "Failed to revert settings - proceeding to reset persistence.";
    }

    remove_file(filepath);
    persistent_data.reset();
  }

  bool
  settings_t::has_persistent_data() const {
    return persistent_data != nullptr;
  }

  bool
  settings_t::is_vdd_in_initial_topology() const {
    if (!persistent_data) {
      return false;
    }

    for (const auto &group : persistent_data->initial_topology) {
      for (const auto &device_id : group) {
        if (device_id == vdd_utils::live_virtual_display_connector()) {
          return true;
        }
      }
    }
    return false;
  }

  void
  settings_t::remove_vdd_from_initial_topology(const std::string &vdd_id) {
    if (!persistent_data) {
      return;
    }

    for (auto &group : persistent_data->initial_topology) {
      group.erase(std::remove(group.begin(), group.end(), vdd_id), group.end());
    }
    persistent_data->initial_topology.erase(
      std::remove_if(persistent_data->initial_topology.begin(), persistent_data->initial_topology.end(),
        [](const std::vector<std::string> &group) { return group.empty(); }),
      persistent_data->initial_topology.end());

    for (auto &group : persistent_data->modified_topology) {
      group.erase(std::remove(group.begin(), group.end(), vdd_id), group.end());
    }
    persistent_data->modified_topology.erase(
      std::remove_if(persistent_data->modified_topology.begin(), persistent_data->modified_topology.end(),
        [](const std::vector<std::string> &group) { return group.empty(); }),
      persistent_data->modified_topology.end());

    persistent_data->original_hdr_states.erase(vdd_id);
    persistent_data->original_modes.erase(vdd_id);

    save_settings(filepath, *persistent_data);
  }

  void
  settings_t::replace_vdd_id(const std::string &old_id, const std::string &new_id) {
    if (!persistent_data) {
      return;
    }

    for (auto &group : persistent_data->initial_topology) {
      std::replace(group.begin(), group.end(), old_id, new_id);
    }
    for (auto &group : persistent_data->modified_topology) {
      std::replace(group.begin(), group.end(), old_id, new_id);
    }

    if (auto it = persistent_data->original_hdr_states.find(old_id); it != persistent_data->original_hdr_states.end()) {
      const auto state = it->second;
      persistent_data->original_hdr_states.erase(it);
      persistent_data->original_hdr_states[new_id] = state;
    }

    if (auto it = persistent_data->original_modes.find(old_id); it != persistent_data->original_modes.end()) {
      const auto mode = it->second;
      persistent_data->original_modes.erase(it);
      persistent_data->original_modes[new_id] = mode;
    }

    save_settings(filepath, *persistent_data);
  }

  std::string
  find_one_of_the_available_devices(const std::string &device_id) {
    // The session flow may hand us a device id, an OS display name or a
    // friendly name; resolve all three against the live connector list. The
    // ZakoVDD friendly name maps to the virtual display when one is live.
    if (device_id.empty()) {
      return {};
    }
    if (device_id == ZAKO_NAME) {
      return vdd_utils::live_virtual_display_connector();
    }

    const auto devices = enum_available_devices();
    if (devices.count(device_id)) {
      return device_id;
    }
    for (const auto &[id, info] : devices) {
      if (info.display_name == device_id || info.friendly_name == device_id) {
        return id;
      }
    }
    return {};
  }

  std::string
  find_device_by_friendlyname(const std::string &friendly_name) {
    if (friendly_name != ZAKO_NAME) {
      return {};
    }
    return vdd_utils::live_virtual_display_connector();
  }

}  // namespace display_device
