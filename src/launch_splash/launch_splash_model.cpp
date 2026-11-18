/**
 * @file src/launch_splash/launch_splash_model.cpp
 * @brief The launch splash's stage vocabulary, status file reader and lifetime rules.
 */
#include "launch_splash_model.h"

#include <algorithm>
#include <utility>

namespace launch_splash {

  namespace {
    struct stage_name_t {
      stage_e stage;
      std::string_view key;
    };

    constexpr stage_name_t stage_names[] = {
      { stage_e::starting, "starting" },
      { stage_e::broker, "broker" },
      { stage_e::preparing, "preparing" },
      { stage_e::injecting, "injecting" },
      { stage_e::waiting_window, "waiting-window" },
      { stage_e::failed, "failed" },
    };

    /// A short code: lowercase letters, digits and dashes. Anything else is dropped, so a launcher cannot put free text in the Panel.
    std::string
    clean_code(const nlohmann::json &value) {
      if (!value.is_string()) {
        return {};
      }
      std::string code = value.get<std::string>();
      if (code.size() > 32) {
        return {};
      }
      for (const char c : code) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
          return {};
        }
      }
      return code;
    }
  }  // namespace

  std::string_view
  stage_key(const stage_e stage) {
    for (const auto &entry : stage_names) {
      if (entry.stage == stage) {
        return entry.key;
      }
    }
    return "starting";
  }

  std::optional<stage_e>
  stage_from_key(const std::string_view key) {
    for (const auto &entry : stage_names) {
      if (entry.key == key) {
        return entry.stage;
      }
    }
    return std::nullopt;
  }

  std::string_view
  kind_key(const kind_e kind) {
    return kind == kind_e::resume ? "resume" : "launch";
  }

  std::optional<launcher_status_t>
  parse_launcher_status(const std::string_view text) {
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object()) {
      return std::nullopt;
    }
    launcher_status_t status;
    if (const auto launch = json.find("launch"); launch != json.end() && launch->is_string()) {
      status.launch = launch->get<std::string>();
    }
    if (const auto stage = json.find("stage"); stage != json.end() && stage->is_string()) {
      status.stage = stage_from_key(stage->get<std::string>());
    }
    if (const auto reason = json.find("reason"); reason != json.end()) {
      status.reason = clean_code(*reason);
    }
    if (const auto notice = json.find("notice"); notice != json.end()) {
      status.notice = clean_code(*notice);
    }
    if (const auto pids = json.find("pids"); pids != json.end() && pids->is_array()) {
      for (const auto &pid : *pids) {
        if (pid.is_number_unsigned() && pid.get<std::uint64_t>() != 0 && pid.get<std::uint64_t>() <= 0xFFFFFFFFull) {
          status.pids.push_back(static_cast<std::uint32_t>(pid.get<std::uint64_t>()));
        }
      }
    }
    return status;
  }

  nlohmann::json
  to_json(const view_t &view) {
    return {
      { "id", view.id },
      { "active", view.active },
      { "kind", kind_key(view.kind) },
      { "app_id", view.app_id },
      { "app_name", view.app_name },
      { "art", view.art },
      { "stage", stage_key(view.stage) },
      { "notice", view.notice },
      { "display_ready", view.display_ready },
      { "pids", view.pids },
      { "started_at_ms", view.started_at_ms },
      { "deadline_ms", view.deadline_ms },
    };
  }

  std::string
  tracker_t::begin(std::string app_id, std::string app_name, std::string art, const kind_e kind, const std::int64_t now_ms, const bool display_ready) {
    view_ = {};
    view_.active = true;
    view_.id = std::to_string(now_ms) + "-" + std::to_string(++counter_);
    view_.kind = kind;
    view_.app_id = std::move(app_id);
    view_.app_name = std::move(app_name);
    view_.art = std::move(art);
    view_.stage = stage_e::starting;
    view_.display_ready = display_ready;
    view_.started_at_ms = now_ms;
    view_.deadline_ms = now_ms + (kind == kind_e::resume ? resume_timeout_ms : timeout_ms);
    notice_until_ms_ = 0;
    return view_.id;
  }

  bool
  tracker_t::set_display_ready() {
    if (!view_.active || view_.display_ready) {
      return false;
    }
    view_.display_ready = true;
    return true;
  }

  void
  tracker_t::add_pid(const std::uint32_t pid) {
    if (!view_.active || pid == 0 || view_.pids.size() >= max_pids) {
      return;
    }
    if (std::find(view_.pids.begin(), view_.pids.end(), pid) == view_.pids.end()) {
      view_.pids.push_back(pid);
    }
  }

  bool
  tracker_t::apply(const launcher_status_t &status, const std::int64_t now_ms) {
    if (!view_.active || view_.stage == stage_e::failed || status.launch != view_.id) {
      return false;
    }
    bool changed = false;
    if (status.stage == stage_e::failed) {
      return fail(status.reason.empty() ? "unknown" : status.reason, now_ms);
    }
    if (status.stage && *status.stage > view_.stage) {
      view_.stage = *status.stage;
      changed = true;
    }
    // a notice is shown for notice_ms from the moment it is first seen; the same code again is the same line
    if (!status.notice.empty() && status.notice != view_.notice) {
      view_.notice = status.notice;
      notice_until_ms_ = now_ms + notice_ms;
      changed = true;
    }
    for (const auto pid : status.pids) {
      const auto before = view_.pids.size();
      add_pid(pid);
      changed = changed || view_.pids.size() != before;
    }
    return changed;
  }

  bool
  tracker_t::fail(std::string reason, const std::int64_t now_ms) {
    if (!view_.active || view_.stage == stage_e::failed) {
      return false;
    }
    view_.stage = stage_e::failed;
    view_.notice = std::move(reason);
    notice_until_ms_ = now_ms + notice_ms;
    view_.deadline_ms = std::min(view_.deadline_ms, now_ms + notice_ms);
    return true;
  }

  bool
  tracker_t::end() {
    if (!view_.active) {
      return false;
    }
    const auto id = view_.id;
    view_ = {};
    view_.id = id;  // the Panel is told this launch is over, not that nothing ever happened
    notice_until_ms_ = 0;
    return true;
  }

  bool
  tracker_t::tick(const std::int64_t now_ms) {
    if (!view_.active) {
      return false;
    }
    if (now_ms >= view_.deadline_ms) {
      return end();
    }
    if (!view_.notice.empty() && view_.stage != stage_e::failed && notice_until_ms_ != 0 && now_ms >= notice_until_ms_) {
      view_.notice.clear();
      notice_until_ms_ = 0;
      return true;
    }
    return false;
  }

}  // namespace launch_splash
