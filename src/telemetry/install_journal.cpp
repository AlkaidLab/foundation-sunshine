/**
 * @file src/telemetry/install_journal.cpp
 */
#include "install_journal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace telemetry::install_journal {

  using nlohmann::json;

  namespace {
    const std::set<std::string_view> COMPONENTS { "host", "vdd", "gamepad", "usbip", "vmouse", "firewall", "service", "autostart", "files", "path", "config", "driver", "panel", "setup" };
    const std::set<std::string_view> PHASES { "install", "upgrade", "repair", "uninstall" };

    struct step_t {
      std::string component;
      std::string step;
      long long exit_code = 0;
      std::int64_t ms = -1;
    };
    struct run_t {
      std::string id;
      std::string phase = "install";
      std::string from;
      std::vector<step_t> steps;
      bool finished = false;
      long long finish_exit = 0;
      std::int64_t finish_ms = -1;
    };

    bool
    run_id_ok(const std::string &id) {
      if (id.empty() || id.size() > 40) return false;
      return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
    }

    std::optional<long long>
    integer_of(const json &j, const char *key) {
      const auto it = j.find(key);
      if (it == j.end() || !it->is_number_integer()) return std::nullopt;
      const long long v = it->get<long long>();
      if (v < -2147483648LL || v > 4294967295LL) return std::nullopt;
      return v;
    }

    std::string
    step_word(const std::string &component, const std::string &raw, long long exit_code) {
      // the root-hub guard of the USB/IP component says which check it failed with its own exit codes 102 to 105
      if (component == "usbip" && exit_code >= 102 && exit_code <= 105) return "roothub_guard";
      auto id = catalogue_id(raw, 32);
      return id.empty() ? "install" : id;
    }
  }  // namespace

  std::string
  error_class_of(long long code) {
    if (code == 5 || code == 1260 || code == 1314) return "permission";
    if (code == 39 || code == 112 || code == 3) return "disk";
    if (code == 1602 || code == 1223) return "cancelled";
    return "other";
  }

  std::string
  exit_code_text(long long code) {
    if (code >= 0) return std::to_string(code);
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08X", static_cast<unsigned>(code & 0xFFFFFFFFLL));
    return buffer;
  }

  std::string
  version_token(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) ++i;
    std::string out;
    for (; i < text.size() && out.size() < 40; ++i) {
      const char c = text[i];
      if ((c >= '0' && c <= '9') || c == '.') out.push_back(c);
      else break;
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
  }

  result_t
  events_from_journal(std::string_view text, std::int64_t age_ms) {
    result_t result;
    std::vector<run_t> runs;
    std::map<std::string, std::size_t> index;
    std::size_t lines = 0;
    if (text.size() > MAX_BYTES) text = text.substr(text.size() - MAX_BYTES);
    std::size_t pos = 0;
    while (pos < text.size() && lines < MAX_LINES) {
      auto end = text.find('\n', pos);
      if (end == std::string_view::npos) end = text.size();
      std::string_view line = text.substr(pos, end - pos);
      pos = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
      if (line.empty()) continue;
      ++lines;
      json j = json::parse(line.begin(), line.end(), nullptr, false);
      if (!j.is_object() || j.value("v", 0) != 1 || !j.contains("kind") || !j["kind"].is_string() || !j.contains("run") || !j["run"].is_string()) continue;
      const auto id = j["run"].get<std::string>();
      if (!run_id_ok(id)) continue;
      auto at = index.find(id);
      if (at == index.end()) {
        if (runs.size() >= MAX_RUNS) continue;
        run_t fresh;
        fresh.id = id;
        runs.push_back(std::move(fresh));
        at = index.emplace(id, runs.size() - 1).first;
      }
      auto &run = runs[at->second];
      const auto kind = j["kind"].get<std::string>();
      if (kind == "start") {
        if (j.contains("phase") && j["phase"].is_string() && PHASES.count(j["phase"].get<std::string>())) run.phase = j["phase"].get<std::string>();
        if (j.contains("from") && j["from"].is_string()) run.from = version_token(j["from"].get<std::string>());
      }
      else if (kind == "component") {
        const auto code = integer_of(j, "exit");
        if (!code || !j.contains("component") || !j["component"].is_string()) continue;
        const auto component = j["component"].get<std::string>();
        if (!COMPONENTS.count(component)) continue;
        step_t step;
        step.component = component;
        step.exit_code = *code;
        step.step = step_word(component, j.contains("step") && j["step"].is_string() ? j["step"].get<std::string>() : std::string {}, *code);
        if (const auto ms = integer_of(j, "ms"); ms && *ms >= 0) step.ms = std::min<long long>(*ms, 7200000);
        // the last report of a component and step wins
        auto same = std::find_if(run.steps.begin(), run.steps.end(), [&](const step_t &s) { return s.component == step.component && s.step == step.step; });
        if (same != run.steps.end()) *same = step;
        else if (run.steps.size() < 32) run.steps.push_back(step);
      }
      else if (kind == "finish") {
        run.finished = true;
        run.finish_exit = integer_of(j, "exit").value_or(0);
        if (const auto ms = integer_of(j, "ms"); ms && *ms >= 0) run.finish_ms = std::min<long long>(*ms, 7200000);
      }
    }
    // a run that is still going (no finish, the journal written a moment ago): wait for the next start
    const bool quiet = age_ms >= QUIET_MS;
    for (const auto &run : runs)
      if (!run.finished && !quiet) {
        result.consumed = false;
        return result;
      }
    for (const auto &run : runs) {
      int failed = 0;
      bool reboot = false;
      for (const auto &s : run.steps) {
        json p = json::object();
        const bool ok = s.exit_code == 0 || s.exit_code == 3010;
        p["result"] = ok ? "ok" : "failed";
        if (!ok) p["error_class"] = error_class_of(s.exit_code);
        p["phase"] = run.phase;
        p["component"] = s.component;
        p["step"] = s.step;
        p["exit_code"] = exit_code_text(s.exit_code);
        if (s.exit_code == 3010) {
          p["reboot_needed"] = true;
          reboot = true;
        }
        if (!run.from.empty()) p["from_version"] = run.from;
        if (s.ms >= 0) p["dur_ms"] = s.ms;
        if (!ok) ++failed;
        result.items.push_back({ kind_e::error, "install_result", std::move(p) });
      }
      json p = json::object();
      p["phase"] = run.phase;
      if (!run.finished) p["result"] = "incomplete";
      else if (run.finish_exit == 1602 || run.finish_exit == 1223) p["result"] = "cancelled";
      else p["result"] = (run.finish_exit == 0 || run.finish_exit == 3010) && failed == 0 ? "ok" : "failed";
      p["components"] = static_cast<int>(run.steps.size());
      p["failed"] = failed;
      if (reboot || run.finish_exit == 3010) p["reboot_needed"] = true;
      if (!run.from.empty()) p["from_version"] = run.from;
      if (run.finish_ms >= 0) p["dur_ms"] = run.finish_ms;
      result.items.push_back({ kind_e::error, "install_summary", std::move(p) });
    }
    return result;
  }

}  // namespace telemetry::install_journal
