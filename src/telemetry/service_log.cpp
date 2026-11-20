#include "service.h"

#include "error_code.h"
#include "log_template.h"

#include <cctype>

using json = nlohmann::json;

namespace telemetry {
  namespace {
    /** The build identity as an allow-list token: lower-case ASCII letters and digits with `.`, `_` and `-`, at most 40 characters
     *  (`v2026.1114.173055.杂鱼` becomes `v2026.1114.173055`). An error event carries it so a report names the build that raised it. */
    std::string
    build_token(std::string_view version) {
      std::string out;
      for (const unsigned char c : version) {
        if (out.size() >= 40) break;
        if (c < 0x80 && std::isalnum(c))
          out.push_back(static_cast<char>(std::tolower(c)));
        else if (!out.empty() && (c == '.' || c == '-') && out.back() != '.' && out.back() != '-')
          out.push_back(static_cast<char>(c));
        else if (!out.empty() && out.back() != '_' && out.back() != '.' && out.back() != '-')
          out.push_back('_');
      }
      while (!out.empty() && !std::isalnum(static_cast<unsigned char>(out.back()))) out.pop_back();
      return out;
    }
  }  // namespace

  void
  service_t::note_log(int severity, std::string_view message_template, std::string_view component, std::string_view site) noexcept {
    if (severity < 3) return;
    try {
      const auto hour = deps_.now_ms() / 3'600'000;
      std::lock_guard lock { log_mutex_ };
      auto &window = log_windows_[std::string { component }];
      if (window.hour != hour && window.hour >= 0 && (window.fatal | window.error | window.warning) == 0) window.hour = hour;
      if (window.hour < 0) window.hour = hour;
      if (severity >= 5)
        ++window.fatal;
      else if (severity == 4)
        ++window.error;
      else
        ++window.warning;
      if (severity < 4) return;
      message_template = message_template.substr(0, MAX_MESSAGE_CHARS * 4);
      auto found = log_errors_.find(std::tie(component, site, message_template));
      if (found == log_errors_.end()) {
        if (log_errors_.size() >= MAX_LOG_ERRORS) return;  // still included in the tally above
        found = log_errors_.try_emplace(log_error_key_t { component, site, message_template }).first;
      }
      ++found->second.count;
      if (severity >= 5) found->second.severity = 5;
    }
    catch (...) {
    }
  }

  void
  service_t::collect_logs(std::int64_t now, bool close_hour) {
    std::map<std::string, log_window_t> windows;
    log_error_table_t errors;
    const auto hour = now / 3'600'000;
    {
      std::lock_guard lock { log_mutex_ };
      for (auto it = log_windows_.begin(); it != log_windows_.end();) {
        auto &w = it->second;
        const bool over = w.hour >= 0 && w.hour != hour;
        if ((over || close_hour) && (w.fatal | w.error | w.warning)) {
          windows[it->first] = w;
          w = log_window_t { hour, 0, 0, 0 };
        }
        ++it;
      }
      errors.swap(log_errors_);
    }
    for (const auto &[component, w] : windows) {
      emit(kind_e::error, "log_summary", json { { "component", catalogue_id(component, 32).empty() ? "host" : catalogue_id(component, 32) }, { "fatal", w.fatal }, { "error", w.error }, { "warning", w.warning } });
    }
    const auto build = build_token(context().version);
    for (const auto &[key, error] : errors) {
      const auto &[source_component, location, message_template] = key;
      const auto site = log_site_basename(location);
      const auto safe_template = scrub_text(message_template, MAX_MESSAGE_CHARS * 4);
      const auto code = template_error_code(safe_template.empty() ? site : safe_template);
      auto component = catalogue_id(source_component, 32);
      if (component.empty()) component = "host";
      const auto severity = error.severity >= 5 ? "fatal" : "error";
      emit(kind_e::error, "error", json { { "component", component }, { "code", code }, { "severity", severity }, { "count", error.count }, { "build", build }, { "site", site } });
      if (const auto loc = clean_frame(location); !loc.empty()) {
        error_input_t input;
        input.component = component;
        input.code = code;
        input.severity = severity;
        input.loc = loc;
        input.frames.push_back(loc);
        report_error(input);
      }
    }
  }
}  // namespace telemetry
