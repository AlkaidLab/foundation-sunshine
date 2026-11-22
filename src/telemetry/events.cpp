/**
 * @file src/telemetry/events.cpp
 * @brief Telemetry v1 event shapes, scrubbing and the batch envelope.
 */
#include "events.h"
#include "error_report.h"
#include "log_template.h"
#include "stream_health.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <regex>
#include <set>

using json = nlohmann::json;
using namespace std::literals;

namespace telemetry {

  // ---------------------------------------------------------------------------------------------------------------------
  // kinds, products

  std::string_view
  kind_name(kind_e kind) {
    if (kind == kind_e::error) return "error"sv;
    if (kind == kind_e::experience) return "experience"sv;
    return "usage"sv;
  }

  std::optional<kind_e>
  parse_kind(std::string_view text) {
    if (text == "usage"sv) return kind_e::usage;
    if (text == "error"sv) return kind_e::error;
    if (text == "experience"sv) return kind_e::experience;
    return std::nullopt;
  }

  consent_e
  consent_of(kind_e kind) {
    return kind == kind_e::error ? consent_e::error_reports : consent_e::usage_statistics;
  }

  int
  event_priority(const event_t &e) {
    if (e.priority >= 0 && e.priority <= 2) return e.priority;
    const auto severe = [&] { return e.props.is_object() && e.props.value("severe", false); };
    if (e.name == "user_report" || e.name == "stream_health") return 0;
    if (e.name == "error_report") return 0;  // the first report of an error; the aggregates of known errors are marked 1 by the sender
    if (e.name == "stream_anomaly") return severe() ? 0 : 1;
    if (e.name == "stream_regress" || e.name == "log_summary") return 1;
    // installs and updates: a failed step, and a run that failed or never finished, are never sampled away; the successes are the denominator of a failure rate
    if (e.name == "install_result" || e.name == "update_result") return e.props.is_object() && e.props.value("result", "") == "failed" ? 0 : 1;
    if (e.name == "install_summary") {
      const auto result = e.props.is_object() ? e.props.value("result", "") : std::string {};
      return result == "failed" || result == "incomplete" ? 0 : 1;
    }
    if (e.name == "device_facts") return 1;
    // a start that failed, a driver that failed and a run that ended badly are what the developers look for: never sampled away. A slow stage and the clipboard record are comparisons: priority 1
    if (e.name == "stream_failed" || e.name == "driver_event" || e.name == "session_abnormal") return 0;
    if (e.name == "stream_stage" || e.name == "clipboard_health") return 1;
    // the service and start-up facts: an anomaly that repeats (or follows an adoption) is never sampled away, the counts and the timings are priority 1
    if (e.name == "host_anomaly") return severe() ? 0 : 1;
    if (e.name == "svc_lifecycle" || e.name == "startup_timing") return 1;
    if (e.name == "error") return e.props.is_object() && e.props.value("severity", "") == "fatal" ? 0 : 1;
    return 2;
  }

  delivery_e
  delivery_of(const event_t &e) {
    if (is_user_report(e.kind, e.name)) return delivery_e::feedback;
    if (e.kind == kind_e::error && event_priority(e) == 0) return delivery_e::error;
    return delivery_e::routine;
  }

  bool
  sampled_family(const event_t &e) {
    if (e.kind == kind_e::error || is_user_report(e.kind, e.name)) return false;
    if (e.name == "stream_health") return true;  // priority 0 by name for the Worker, but a summary of every stream: a sample of the installs is enough
    return event_priority(e) >= 1;
  }

  bool
  stage_is_slow(std::string_view stage, std::int64_t ms) {
    const std::int64_t bound = stage == "app_launch"sv || stage == "first_frame"sv ? 5000 : 3000;
    return ms > bound;
  }

  bool
  known_product(std::string_view product) {
    return product == "host"sv || product == "panel"sv || product == "web"sv || product == "sparxie-launch"sv;
  }

  bool
  client_may_send(std::string_view product, kind_e kind, std::string_view name) {
    if (product != "panel"sv && product != "web"sv && product != "sparxie-launch"sv) return false;
    if (kind == kind_e::usage) return name == "app_start"sv || name == "app_exit"sv || name == "feature_used"sv || name == "setting_changed"sv;
    // "user_report" is a report the person wrote in the error dialog; it is kind error and follows the error-report choice like every error event
    // the result of an update step is the page's to report (it runs the update sheet); install results and device facts are the host's own, read from the installer's journal and the system inventory
    if (kind == kind_e::error) return name == "error"sv || name == "user_report"sv || name == "error_report"sv || name == "update_result"sv;
    // the sketches of a page (page_vitals) and the latency of its calls to the host (api_latency)
    // page_ready: the page says when it was ready; the host judges it (service_lifecycle.h) and does not pass the raw event on
    return name == "panel_page"sv || name == "startup"sv || name == "page_vitals"sv || name == "api_latency"sv || name == "page_ready"sv || (product == "panel"sv && name == "clipboard_health"sv);
  }

  // ---------------------------------------------------------------------------------------------------------------------
  // scrubbing

  namespace {
    // Code point count and cut, so a multi-byte character is never split.
    std::string
    cut_utf8(const std::string &text, std::size_t max_chars) {
      std::size_t chars = 0, i = 0;
      while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 :
                                                (lead >> 4) == 0xe   ? 3 :
                                                (lead >> 3) == 0x1e  ? 4 :
                                                                       1;
        if (chars == max_chars) return text.substr(0, i);
        i += std::min(len, text.size() - i);
        ++chars;
      }
      return text;
    }

    std::string
    drop_controls(const std::string &text) {
      std::string out;
      out.reserve(text.size());
      for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\t' || c == '\n' || c == '\r') {
          out.push_back(' ');
          continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        // C1 controls (U+0080..U+009F) are 0xC2 0x80..0x9F; U+2028/2029 are E2 80 A8/A9; BOM EF BB BF; bidi controls E2 80 AA..AE and E2 81 A6..A9
        if (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 && static_cast<unsigned char>(text[i + 1]) <= 0x9f) {
          ++i;
          continue;
        }
        if (c == 0xe2 && i + 2 < text.size()) {
          const auto b = static_cast<unsigned char>(text[i + 1]);
          const auto d = static_cast<unsigned char>(text[i + 2]);
          if ((b == 0x80 && d >= 0xa8 && d <= 0xae) || (b == 0x81 && d >= 0xa6 && d <= 0xa9)) {
            i += 2;
            continue;
          }
        }
        if (c == 0xef && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xbb && static_cast<unsigned char>(text[i + 2]) == 0xbf) {
          i += 2;
          continue;
        }
        out.push_back(static_cast<char>(c));
      }
      return out;
    }

    std::string
    replace_fn(const std::string &text, const std::regex &re, const std::function<std::string(const std::smatch &)> &fn) {
      std::string out;
      auto begin = std::sregex_iterator(text.begin(), text.end(), re);
      std::size_t last = 0;
      for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const auto &m = *it;
        out.append(text, last, static_cast<std::size_t>(m.position(0)) - last);
        out += fn(m);
        last = static_cast<std::size_t>(m.position(0) + m.length(0));
      }
      out.append(text, last, std::string::npos);
      return out;
    }

    std::string
    replace_all(const std::string &text, const std::regex &re, const std::string &with) {
      return std::regex_replace(text, re, with);
    }

    bool
    valid_ipv6(const std::string &s) {
      // eight groups, or a compressed form with one "::"; only hex groups of at most four digits. To tell an address from text such as `ab::cd` it needs a decimal digit (fe80::1) or a group of
      // at least three hex digits (fdab::cafe): a short word between colons is not an address, a long one is
      if (s.find(":::") != std::string::npos) return false;
      bool digit = false;
      for (char c : s) digit = digit || (c >= '0' && c <= '9');
      if (!digit) {
        std::size_t longest = 0, run = 0;
        for (char c : s) {
          run = c == ':' ? 0 : run + 1;
          longest = std::max(longest, run);
        }
        if (longest < 3) return false;
      }
      const auto dbl = s.find("::");
      if (dbl != std::string::npos && s.find("::", dbl + 1) != std::string::npos) return false;
      std::size_t groups = 0, start = 0;
      for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ':') {
          const auto len = i - start;
          if (len > 4) return false;
          if (len > 0) ++groups;
          start = i + 1;
        }
      }
      return dbl != std::string::npos ? groups <= 7 : groups == 8;
    }
  }  // namespace

  std::string
  scrub_text(std::string_view input, std::size_t max) {
    std::string text { input.substr(0, max * 4) };
    static const std::regex origin(R"(\bhttps?://[^\s/"')\]]+)", std::regex::icase);
    // a home folder whose account name may hold spaces ("Sky Hua"): the whole path goes, not the first word of it
    static const std::regex win_users_path(R"(\b[A-Za-z]:[\\/]Users[\\/](?:[^\\/"'<>|\r\n]+(?=[\\/])|[^\\/\s"'<>|]+)(?:[\\/][^\s"'<>|]*)*)");
    static const std::regex unix_users_path(R"(/(?:Users|home)/(?:[^/"'<>|\r\n]+(?=/)|[^/\s"'<>|]+)(?:/[^\s"'<>|]*)*)");
    static const std::regex sid(R"(\bS-1-\d+(?:-\d+){2,}\b)");
    static const std::regex steam_id(R"(\b7656119\d{10}\b)");
    static const std::regex default_computer(R"(\b(?:DESKTOP|LAPTOP|WIN)-[A-Z0-9]{6,12}\b)");
    static const std::regex win_home(R"(\b([A-Za-z]):[\\/]Users[\\/][^\\/\s"'<>|]+)");
    static const std::regex unix_users(R"(/Users/[^/\s"'<>]+)");
    static const std::regex unix_home(R"(/home/[^/\s"'<>]+)");
    static const std::regex unc(R"(\\\\[^\s\\"'<>|]+(?:\\[^\s"'<>|]*)*)");
    static const std::regex win_path(R"(\b[A-Za-z]:[\\/][^\s"'<>|]*)");
    static const std::regex unix_path(R"((^|[\s"'(=\[])(/(?:[^\s/"')\]]+/)+[^\s"')\]]*))");
    static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    static const std::regex ipv4(R"(\b(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}\b)");
    static const std::regex mac(R"(\b(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b)");
    static const std::regex v6cand(R"([0-9A-Fa-f:]{3,})");
    static const std::regex uuid(R"(\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}\b)");
    static const std::regex sk(R"(\bsk-[A-Za-z0-9_-]{12,}\b)");
    static const std::regex scheme(R"(\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{8,})");
    static const std::regex secret(R"(\b(token|password|passwd|pwd|secret|api[_-]?key|apikey|auth(?:orization)?|cookie|session|pin|cdk|credential)(["']?\s*[:=]\s*["']?)[^\s&"',;]+)", std::regex::icase);
    static const std::regex query(R"(([?&][A-Za-z0-9_.-]+)=[^&\s#"']*)");
    static const std::regex host_name(R"(\b[A-Za-z0-9-]+\.(?:local|lan|home|internal|localdomain|home\.arpa)\b)", std::regex::icase);
    static const std::regex long_id(R"(\b[A-Za-z0-9_-]{32,}\b)");

    text = replace_all(text, origin, "<origin>");
    text = replace_all(text, win_users_path, "<path>");
    text = replace_all(text, unix_users_path, "<path>");
    // whole paths first (a path names folders a person made), then what is left of a home folder
    text = replace_all(text, unc, "<path>");
    text = replace_all(text, win_path, "<path>");
    text = replace_all(text, unix_path, "$1<path>");
    text = replace_all(text, win_home, "$1:\\Users\\<user>");
    text = replace_all(text, unix_users, "/Users/<user>");
    text = replace_all(text, unix_home, "/home/<user>");
    text = replace_all(text, email, "<email>");
    text = replace_fn(text, ipv4, [&](const std::smatch &m) {
      const auto pos = static_cast<std::size_t>(m.position(0));
      const auto from = pos > 12 ? pos - 12 : 0;
      std::string before = text.substr(from, pos - from);
      std::transform(before.begin(), before.end(), before.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      static const std::regex ver(R"((version|ver\.?|v|driver|build)[\s:=/_-]*$)");
      return std::regex_search(before, ver) ? m.str(0) : std::string { "<ip>" };
    });
    text = replace_all(text, mac, "<mac>");
    text = replace_fn(text, v6cand, [&](const std::smatch &m) {
      const auto pos = static_cast<std::size_t>(m.position(0));
      const auto end = pos + static_cast<std::size_t>(m.length(0));
      const bool before_word = pos > 0 && (std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '.' || text[pos - 1] == '_');
      const bool after_word = end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '.' || text[end] == '_');
      return (!before_word && !after_word && valid_ipv6(m.str(0))) ? std::string { "<ipv6>" } : m.str(0);
    });
    text = replace_all(text, uuid, "<id>");
    text = replace_all(text, sid, "<sid>");
    text = replace_all(text, steam_id, "<id>");
    text = replace_all(text, default_computer, "<host>");
    text = replace_all(text, sk, "<hidden>");
    text = replace_all(text, scheme, "$1 <hidden>");
    text = replace_all(text, secret, "$1$2<hidden>");
    text = replace_all(text, query, "$1=…");
    text = replace_all(text, host_name, "<host>");
    text = replace_all(text, long_id, "<id>");
    text = drop_controls(text);
    return cut_utf8(text, max);
  }

  std::string
  scrub_log_message(std::string_view input, std::size_t max) {
    // A log line names things the person chose (a device, an application, a game, a window title) between quotes or brackets: what stands between them is not part of the message.
    // A quote or bracket that has no partner is not trusted: the rest of the line goes, because the name may run on past where a closing mark seems to be.
    const std::string text { input.substr(0, max * 4) };
    const auto n = text.size();
    constexpr std::string_view omitted = "<…>";
    // an apostrophe inside a word ("Couldn't") stays; a single quote opens a name only at the start of a word
    const auto opens_word = [&](std::size_t p) {
      return p == 0 || std::isspace(static_cast<unsigned char>(text[p - 1])) || text[p - 1] == '(' || text[p - 1] == '=' || text[p - 1] == ':' || text[p - 1] == ',';
    };
    const auto may_close = [&](std::size_t p) {
      return p + 1 == n || std::isspace(static_cast<unsigned char>(text[p + 1])) || std::string_view { ").,;:!?]" }.find(text[p + 1]) != std::string_view::npos;
    };
    // keep a bracket that only holds numbers or a short upper-case tag (a code, a count), drop the rest
    static const std::regex plain(R"(^[A-Z0-9_ .:=,-]{0,24}$)");
    std::string out;
    out.reserve(n);
    std::size_t i = 0;
    while (i < n) {
      const char c = text[i];
      if (c == '"' || c == '[' || c == '{') {
        std::size_t j = std::string::npos;
        if (c == '"') {
          j = text.find('"', i + 1);
        }
        else {
          // a bracket may hold brackets ("[Project [draft] Budget.xlsx]"): the name ends where the outermost one closes
          int depth = 0;
          for (std::size_t k = i; k < n; ++k) {
            if (text[k] == '[' || text[k] == '{') {
              ++depth;
            }
            else if ((text[k] == ']' || text[k] == '}') && --depth == 0) {
              j = k;
              break;
            }
          }
        }
        if (j == std::string::npos) {
          out += omitted;
          break;
        }
        const auto inner = text.substr(i + 1, j - i - 1);
        if (c == '[' && std::regex_match(inner, plain)) {
          out.append(text, i, j - i + 1);
        }
        else {
          out += omitted;
        }
        i = j + 1;
        continue;
      }
      if (c == '\'' && opens_word(i)) {
        // the name ends at the last quote that could close it: a title can hold a quote followed by a space
        std::size_t last = std::string::npos;
        for (std::size_t k = i + 1; k < n; ++k) {
          if (text[k] == '\'' && may_close(k)) last = k;
        }
        out += omitted;
        if (last == std::string::npos) break;
        i = last + 1;
        continue;
      }
      out.push_back(c);
      ++i;
    }
    return scrub_text(out, max);
  }

  std::string
  catalogue_id(std::string_view text, std::size_t max) {
    if (text.empty() || text.size() > max) return {};
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '.' || c == '_' || c == '-'));
      if (!ok) return {};
    }
    return std::string { text };
  }

  // ---------------------------------------------------------------------------------------------------------------------
  // allow-list per event name

  namespace {
    enum class type_e { boolean,
      integer,
      number,
      choice,
      catalogue,
      code,
      attachment,
      log_ref,
      shot_ref,
      reply_key,
      text,
      contact,
      histograms,
      counts,
      frames,
      crumbs,
      log_lines,
      frame,
      site,
      vector8 };  // exactly 8 non-negative integers: the fixed buckets of a page sketch

    struct prop_spec_t {
      std::string_view key;
      type_e type;
      double min = 0;
      double max = 0;
      std::vector<std::string_view> choices {};
      bool required = false;
    };

    struct event_spec_t {
      kind_e kind;
      std::string_view name;
      std::vector<prop_spec_t> props;
    };

    const std::vector<std::string_view> os_choices { "windows", "macos", "linux", "ios", "ipados", "android", "tvos", "other" };
    // the Worker takes h264 | hevc | av1 | other (the experimental formats count as other)
    const std::vector<std::string_view> codec_choices { "h264", "hevc", "av1", "other" };
    const std::vector<std::string_view> transport_choices { "enet", "altp", "other" };
    const std::vector<std::string_view> app_kind_choices { "desktop", "bigscreen", "sparxie", "steam", "other" };
    const std::vector<std::string_view> result_choices { "normal", "error", "timeout", "kicked" };
    const std::vector<std::string_view> severity_choices { "info", "warning", "error", "fatal" };
    const std::vector<std::string_view> report_category_choices { "bug", "performance", "compat", "suggestion", "other" };
    const std::vector<std::string_view> opt_out_choices { "usage", "errors" };
    // why a feedback report went without its screenshot (src/telemetry/service.cpp, ingest): the words are the Worker's, `uploaded` is the one that has a shot_ref
    const std::vector<std::string_view> shot_state_choices { "none", "too_large", "rejected", "unreachable", "refused", "bad_answer", "unavailable", "uploaded" };
    const std::vector<std::string_view> log_state_choices { "no_log", "pack_failed", "refused", "unreachable", "bad_answer", "errors_off", "uploaded" };
    const std::vector<std::string_view> restart_reason_choices { "settings", "guide", "troubleshooting", "doctor", "home", "shell", "tray", "widget", "import", "api" };
    const std::vector<std::string_view> update_source_choices { "mirror", "cnb", "github", "other" };
    // why the chosen update source (Mirror酱) was not the one used in the last download
    const std::vector<std::string_view> update_source_miss_choices { "version_lag", "cdk_invalid", "quota", "network", "no_cdk" };
    const std::vector<std::string_view> update_result_source_choices { "mirror", "cnb", "github", "manual", "other" };
    const std::vector<std::string_view> update_stage_choices { "check", "download", "verify", "apply", "restart" };
    const std::vector<std::string_view> ok_failed_choices { "ok", "failed" };
    const std::vector<std::string_view> run_result_choices { "ok", "failed", "cancelled", "incomplete" };
    const std::vector<std::string_view> error_class_choices { "network", "http_4xx", "http_5xx", "hash_mismatch", "disk", "permission", "cancelled", "other" };
    const std::vector<std::string_view> install_phase_choices { "install", "upgrade", "repair", "uninstall" };
    const std::vector<std::string_view> ready_which_choices { "svc_core", "page", "core" };
    const std::vector<std::string_view> host_anomaly_codes { "XP_SVC_CORE_RESTART", "XP_SLOW_READY" };
    const std::vector<std::string_view> host_anomaly_causes { "soon_after_adopt", "repeat", "unexpected_exit", "above_limit", "above_baseline", "other" };
    // stream_end: what the clipboard did in the session. `clip_offer` is the decision made when the session was set up (clipboard_bridge::offer_name), `clip_flow` what the host saw afterwards
    const std::vector<std::string_view> clip_offer_choices { "offered", "off_config", "no_agent", "agent_stale", "unknown" };
    const std::vector<std::string_view> clip_flow_choices { "none", "ok", "dropped" };
    // the fixed stage words of a stream's start (the Worker's STREAM_STAGES)
    const std::vector<std::string_view> stream_stage_choices { "launch_accept", "display_prepare", "vdd_create", "app_launch", "capture_init", "encoder_init", "rtsp_setup", "first_frame" };
    const std::vector<std::string_view> driver_choices { "vdd", "gamepad", "microphone", "usbip", "other" };
    const std::vector<std::string_view> driver_op_choices { "create", "remove", "start", "stop", "probe", "other" };
    const std::vector<std::string_view> feedback_category_choices { "bug", "feature", "improvement", "other" };
    const std::vector<std::string_view> clip_error_choices { "none", "connect_refused", "timeout", "tls", "http_401", "http_403", "http_5xx", "stream_ended", "credential_missing", "heartbeat", "other" };
    const std::vector<std::string_view> clip_conn_choices { "lt50", "50_90", "gt90" };
    const std::vector<std::string_view> nic_link_choices { "wired", "wireless", "other", "unknown" };
    const std::vector<std::string_view> nic_speed_choices { "lt_1g", "1g", "2_5g", "5g", "10g", "gt_10g", "unknown" };

    std::vector<prop_spec_t>
    stream_dims() {
      return {
        { "client_kind", type_e::catalogue, 0, 40 },
        { "client_os", type_e::choice, 0, 0, os_choices },
        { "codec", type_e::choice, 0, 0, codec_choices },
        { "width", type_e::integer, 0, 16384 },
        { "height", type_e::integer, 0, 16384 },
        { "fps", type_e::integer, 0, 1000 },
        { "hdr", type_e::boolean },
        { "audio_codec", type_e::choice, 0, 0, { "opus", "other" } },
        { "transport", type_e::choice, 0, 0, transport_choices },
        { "app_kind", type_e::choice, 0, 0, app_kind_choices },
        { "sparxie_game", type_e::catalogue, 0, 40 },
      };
    }

    // the dimensions of the stream-health events: no audio codec, app kind or game (the Worker takes the same list)
    std::vector<prop_spec_t>
    slim_dims() {
      return {
        { "client_kind", type_e::catalogue, 0, 40 },
        { "client_os", type_e::choice, 0, 0, os_choices },
        { "codec", type_e::choice, 0, 0, codec_choices },
        { "width", type_e::integer, 0, 16384 },
        { "height", type_e::integer, 0, 16384 },
        { "fps", type_e::integer, 0, 1000 },
        { "hdr", type_e::boolean },
        { "transport", type_e::choice, 0, 0, transport_choices },
      };
    }

    const std::vector<event_spec_t> &
    specs() {
      static const std::vector<event_spec_t> table = [] {
        std::vector<event_spec_t> t;
        t.push_back({ kind_e::usage, "app_start", { { "cold", type_e::boolean }, { "start_ms", type_e::integer, 0, 600000 }, { "update_source", type_e::choice, 0, 0, update_source_choices }, { "update_source_miss", type_e::choice, 0, 0, update_source_miss_choices } } });
        t.push_back({ kind_e::usage, "app_exit", { { "uptime_s", type_e::integer, 0, 31536000 } } });
        // the one record that a choice was turned off (opt_out.h): which choice, nothing else; the host sends it itself, a page cannot
        t.push_back({ kind_e::usage, "opt_out", { { "choice", type_e::choice, 0, 0, opt_out_choices, true } } });
        t.push_back({ kind_e::usage, "stream_start", stream_dims() });
        auto end = stream_dims();
        end.push_back({ "duration_s", type_e::integer, 0, 604800 });
        end.push_back({ "result", type_e::choice, 0, 0, result_choices, true });
        end.push_back({ "clip_offer", type_e::choice, 0, 0, clip_offer_choices });
        end.push_back({ "clip_flow", type_e::choice, 0, 0, clip_flow_choices });
        t.push_back({ kind_e::usage, "stream_end", end });
        t.push_back({ kind_e::usage, "feature_used", { { "feature", type_e::catalogue, 0, 48, {}, true } } });
        t.push_back({ kind_e::usage, "setting_changed", { { "key", type_e::catalogue, 0, 48, {}, true }, { "value", type_e::catalogue, 0, 32, {}, true } } });
        t.push_back({ kind_e::error, "error", {
                                                { "component", type_e::catalogue, 0, 32, {}, true },
                                                { "code", type_e::code, 0, 64 },
                                                { "severity", type_e::choice, 0, 0, severity_choices },
                                                { "count", type_e::integer, 1, 1000000 },
                                                { "build", type_e::catalogue, 0, 40 },
                                                { "site", type_e::site },  // basename and line only
                                              } });
        // what the person wrote in the error dialog (spec "Worker additions"): a category, the component and page as tokens, an error code, the reference of an uploaded attachment and one scrubbed message of at most 2000 characters, the screenshot's reference and the word for why it is missing; the contact when the person left one
        t.push_back({ kind_e::error, "user_report", {
                                                      { "category", type_e::choice, 0, 0, report_category_choices },
                                                      { "component", type_e::catalogue, 0, 32, {}, true },
                                                      { "page", type_e::catalogue, 0, 48 },
                                                      { "error_code", type_e::code, 0, 64 },
                                                      { "attachment", type_e::attachment },
                                                      { "log_ref", type_e::log_ref },
                                                      { "log_state", type_e::choice, 0, 0, log_state_choices },
                                                      { "shot_ref", type_e::shot_ref },
                                                      { "shot_state", type_e::choice, 0, 0, shot_state_choices },
                                                      { "message", type_e::text, 0, static_cast<double>(MAX_USER_REPORT_CHARS), {}, true },
                                                      { "contact", type_e::contact, 0, static_cast<double>(MAX_USER_CONTACT_CHARS) },
                                                      { "reply_key", type_e::reply_key },
                                                    } });
        // a fault of the virtual display driver that Windows Error Reporting wrote to the Application log: that it happened, which module, the exception code and the offset (wer_event.h); no stack exists
        t.push_back({ kind_e::error, "wer_event", {
                                                    { "event_id", type_e::choice, 0, 0, { "1000", "1001", "1002" }, true },
                                                    { "kind", type_e::choice, 0, 0, { "crash", "hang" }, true },
                                                    { "module", type_e::catalogue, 0, 96, {}, true },
                                                    { "exception_code", type_e::code, 0, 64 },
                                                    { "offset", type_e::code, 0, 64 },
                                                  } });
        t.push_back({ kind_e::error, "log_summary", {
                                                      { "component", type_e::catalogue, 0, 32, {}, true },
                                                      { "fatal", type_e::integer, 0, 1000000 },
                                                      { "error", type_e::integer, 0, 1000000 },
                                                      { "warning", type_e::integer, 0, 1000000 },
                                                    } });
        t.push_back({ kind_e::experience, "stream_quality", {
                                                              { "fps_p50", type_e::number, 0, 1000 },
                                                              { "fps_p5", type_e::number, 0, 1000 },
                                                              { "frame_drop_pct", type_e::number, 0, 100 },
                                                              { "encode_ms_p50", type_e::number, 0, 10000 },
                                                              { "encode_ms_p95", type_e::number, 0, 10000 },
                                                              { "capture_ms_p50", type_e::number, 0, 10000 },
                                                              { "capture_ms_p95", type_e::number, 0, 10000 },
                                                              { "network_rtt_ms_p50", type_e::number, 0, 60000 },
                                                              { "network_rtt_ms_p95", type_e::number, 0, 60000 },
                                                              { "loss_pct", type_e::number, 0, 100 },
                                                              { "bitrate_kbps_avg", type_e::integer, 0, 2000000 },
                                                              { "reconnects", type_e::integer, 0, 10000 },
                                                              { "stalls", type_e::integer, 0, 100000 },
                                                              // the dimensions the reports split by (codec) and the length of the session; not in the contract's list, proposed to the controller
                                                              { "codec", type_e::choice, 0, 0, codec_choices },
                                                              { "transport", type_e::choice, 0, 0, transport_choices },
                                                              { "duration_s", type_e::integer, 0, 604800 },
                                                              { "encoder", type_e::catalogue, 0, 48 },
                                                              { "capture_backend", type_e::catalogue, 0, 48 },
                                                            } });
        // One chunk of stream-health windows (stream_health.h): fixed-bucket histograms of the per-window statistics, counters, and the stream's dimensions as fixed words.
        {
          std::vector<prop_spec_t> health {
            { "h", type_e::histograms },
            { "bad", type_e::counts },
            { "windows", type_e::integer, 0, 100000 },
            { "idle_windows", type_e::integer, 0, 100000 },
            { "window_s", type_e::integer, 0, 8640000 },
            { "frames", type_e::integer, 0, 100000000 },
            { "captured", type_e::integer, 0, 100000000 },
            { "submitted", type_e::integer, 0, 100000000 },
            { "queue_flushes", type_e::integer, 0, 1000000 },
            { "queue_frames_dropped", type_e::integer, 0, 100000000 },
            { "send_would_block_packets", type_e::integer, 0, 1000000000 },
            { "idr_requests", type_e::integer, 0, 1000000 },
            { "rfi_requests", type_e::integer, 0, 1000000 },
            { "anomalies", type_e::integer, 0, 1000000 },
            { "loss_pct_avg", type_e::number, 0, 100 },
            { "loss_pct_max", type_e::number, 0, 100 },
            { "rtt_ms_p50", type_e::number, 0, 60000 },
            { "rtt_ms_p95", type_e::number, 0, 60000 },
            { "fec_pct_avg", type_e::number, 0, 100 },
            { "actual_kbps", type_e::integer, 0, 10000000 },
            { "target_kbps", type_e::integer, 0, 10000000 },
            { "bitrate_band", type_e::choice, 0, 0, { "0-8000", "8000-15000", "15000-30000", "30000-60000", "60000-100000", "100000-200000", "200000+" } },
            { "encoder", type_e::catalogue, 0, 48 },
            { "capture_backend", type_e::catalogue, 0, 48 },
            { "gpu_driver", type_e::catalogue, 0, 24 },
          };
          for (auto &d : slim_dims()) health.push_back(d);
          t.push_back({ kind_e::experience, "stream_health", std::move(health) });
          // One anomaly of a window: the code and its numbers say what happened; `log` says an anonymized log excerpt was sent for it.
          std::vector<prop_spec_t> anomaly {
            { "code", type_e::code, 0, 32, {}, true },
            { "severe", type_e::boolean },
            { "value", type_e::number, 0, 10000000 },
            { "threshold", type_e::number, 0, 10000000 },
            { "count", type_e::integer, 0, 100000000 },
            { "suppressed", type_e::integer, 0, 1000000 },
            { "window_s", type_e::integer, 0, 3600 },
            { "log", type_e::boolean },
            { "bitrate_band", type_e::choice, 0, 0, { "0-8000", "8000-15000", "15000-30000", "30000-60000", "60000-100000", "100000-200000", "200000+" } },
            { "encoder", type_e::catalogue, 0, 48 },
            { "capture_backend", type_e::catalogue, 0, 48 },
            { "gpu_driver", type_e::catalogue, 0, 24 },
          };
          for (auto &d : slim_dims()) anomaly.push_back(d);
          t.push_back({ kind_e::experience, "stream_anomaly", std::move(anomaly) });
          // an install found one of its own statistics clearly worse than its own baseline (self_baseline.h)
          std::vector<prop_spec_t> regress {
            { "metric", type_e::catalogue, 0, 24, {}, true },
            { "baseline", type_e::number, 0, 100000 },
            { "current", type_e::number, 0, 100000 },
            { "delta", type_e::number, 0, 100000 },
            { "windows", type_e::integer, 0, 100000 },
            { "baseline_windows", type_e::integer, 0, 10000000 },
            { "version_changed", type_e::boolean },
            { "bitrate_band", type_e::choice, 0, 0, { "0-8000", "8000-15000", "15000-30000", "30000-60000", "60000-100000", "100000-200000", "200000+" } },
            { "encoder", type_e::catalogue, 0, 48 },
            { "capture_backend", type_e::catalogue, 0, 48 },
            { "gpu_driver", type_e::catalogue, 0, 24 },
          };
          for (auto &d : slim_dims()) regress.push_back(d);
          t.push_back({ kind_e::experience, "stream_regress", std::move(regress) });
        }
        // An error of any component: code, location, stack, fingerprint and the user's last actions (error_report.h); the Worker groups by component and fingerprint
        t.push_back({ kind_e::error, "error_report", {
                                                       { "component", type_e::catalogue, 0, 32, {}, true },
                                                       { "lang", type_e::choice, 0, 0, { "cpp", "js", "rust", "dotnet", "installer", "driver" } },
                                                       { "fingerprint", type_e::catalogue, 0, 64, {}, true },
                                                       { "code", type_e::code, 0, 64, {}, true },
                                                       { "severity", type_e::choice, 0, 0, severity_choices },
                                                       { "loc", type_e::frame },
                                                       { "build", type_e::catalogue, 0, 40 },
                                                       { "page", type_e::catalogue, 0, 48 },
                                                       { "count", type_e::integer, 1, 1000000 },
                                                       { "frames", type_e::frames },
                                                       { "crumbs", type_e::crumbs },
                                                       { "log", type_e::log_lines },
                                                     } });
        t.push_back({ kind_e::experience, "startup", { { "host_start_ms", type_e::integer, 0, 3600000 }, { "first_frame_ms", type_e::integer, 0, 3600000 } } });
        t.push_back({ kind_e::experience, "panel_page", { { "page", type_e::catalogue, 0, 40, {}, true }, { "time_to_content_ms", type_e::integer, 0, 600000, {}, true } } });
        // Stage (b): the sketches of a page, one per route and send: eight bucket counts per metric (foundation-ui pageBuckets.js; the Worker has the same bounds) and a few counters. No trace, no text.
        t.push_back({ kind_e::experience, "page_vitals", {
                                                           { "page", type_e::catalogue, 0, 48, {}, true },
                                                           { "visits", type_e::integer, 0, 100000 },
                                                           { "lcp", type_e::vector8 },
                                                           { "inp", type_e::vector8 },
                                                           { "cls", type_e::vector8 },
                                                           { "loaf", type_e::vector8 },
                                                           { "fpaint", type_e::vector8 },
                                                           { "tti", type_e::vector8 },
                                                           { "heap", type_e::vector8 },
                                                           { "loaf_n", type_e::integer, 0, 10000000 },
                                                           { "loaf_block_ms", type_e::integer, 0, 100000000 },
                                                           { "fr_a", type_e::integer, 0, 100000000 },
                                                           { "fr_b", type_e::integer, 0, 100000000 },
                                                           { "fr_c", type_e::integer, 0, 100000000 },
                                                           { "fr_d", type_e::integer, 0, 100000000 },
                                                           { "dwell_s", type_e::integer, 0, 8640000 },
                                                         } });
        // the latency of the page's calls to this host, per endpoint word: calls, failures, timeouts and eight latency buckets
        t.push_back({ kind_e::experience, "api_latency", {
                                                           { "endpoint", type_e::catalogue, 0, 32, {}, true },
                                                           { "calls", type_e::integer, 0, 10000000 },
                                                           { "errors", type_e::integer, 0, 10000000 },
                                                           { "timeouts", type_e::integer, 0, 10000000 },
                                                           { "lat", type_e::vector8 },
                                                         } });
        // Stage (c): the result of an update step (the page runs the update sheet), one component of an install run and the run itself (the host reads the installer's journal), and coarse device facts
        t.push_back({ kind_e::error, "update_result", {
                                                        { "source", type_e::choice, 0, 0, update_result_source_choices, true },
                                                        { "stage", type_e::choice, 0, 0, update_stage_choices, true },
                                                        { "result", type_e::choice, 0, 0, ok_failed_choices, true },
                                                        { "error_class", type_e::choice, 0, 0, error_class_choices },
                                                        { "from_version", type_e::catalogue, 0, 40 },
                                                        { "to_version", type_e::catalogue, 0, 40 },
                                                        { "dur_ms", type_e::integer, 0, 3600000 },
                                                        { "reboot_needed", type_e::boolean },
                                                      } });
        t.push_back({ kind_e::error, "install_result", {
                                                         { "result", type_e::choice, 0, 0, ok_failed_choices, true },
                                                         { "error_class", type_e::choice, 0, 0, error_class_choices },
                                                         { "phase", type_e::choice, 0, 0, install_phase_choices },
                                                         { "component", type_e::catalogue, 0, 24 },
                                                         { "step", type_e::catalogue, 0, 32 },
                                                         { "exit_code", type_e::code, 0, 24 },
                                                         { "reboot_needed", type_e::boolean },
                                                         { "from_version", type_e::catalogue, 0, 40 },
                                                         { "dur_ms", type_e::integer, 0, 7200000 },
                                                       } });
        t.push_back({ kind_e::error, "install_summary", {
                                                          { "phase", type_e::choice, 0, 0, install_phase_choices },
                                                          { "result", type_e::choice, 0, 0, run_result_choices, true },
                                                          { "components", type_e::integer, 0, 64 },
                                                          { "failed", type_e::integer, 0, 64 },
                                                          { "reboot_needed", type_e::boolean },
                                                          { "from_version", type_e::catalogue, 0, 40 },
                                                          { "dur_ms", type_e::integer, 0, 7200000 },
                                                        } });
        // Service and start-up facts (service_lifecycle.h): the counts of what the service did with the core, the time to ready, and the anomalies XP_SVC_CORE_RESTART and XP_SLOW_READY
        t.push_back({ kind_e::experience, "svc_lifecycle", {
                                                             { "launched", type_e::integer, 0, 1000 },
                                                             { "adopted", type_e::integer, 0, 1000 },
                                                             { "extras_ended", type_e::integer, 0, 1000 },
                                                             { "exits", type_e::integer, 0, 1000 },
                                                             { "restarts_requested", type_e::integer, 0, 1000 },
                                                             { "restart_reason", type_e::choice, 0, 0, restart_reason_choices },
                                                             { "unexpected", type_e::integer, 0, 1000 },
                                                             { "soon_after_adopt", type_e::integer, 0, 1000 },
                                                             { "restarts_core_exit", type_e::integer, 0, 1000 },
                                                             { "restarts_session", type_e::integer, 0, 1000 },
                                                           } });
        t.push_back({ kind_e::experience, "startup_timing", {
                                                              { "which", type_e::choice, 0, 0, ready_which_choices, true },
                                                              { "ms", type_e::integer, 0, 3600000, {}, true },
                                                              { "slow", type_e::boolean },
                                                            } });
        t.push_back({ kind_e::experience, "host_anomaly", {
                                                            { "code", type_e::choice, 0, 0, host_anomaly_codes, true },
                                                            { "severe", type_e::boolean },
                                                            { "value", type_e::number, 0, 3600000000.0 },
                                                            { "threshold", type_e::number, 0, 3600000000.0 },
                                                            { "count", type_e::integer, 1, 1000000 },
                                                            { "cause", type_e::choice, 0, 0, host_anomaly_causes },
                                                            { "which", type_e::choice, 0, 0, ready_which_choices },
                                                            { "exit_code", type_e::code, 0, 24 },
                                                          } });
        t.push_back({ kind_e::experience, "page_ready", { { "page", type_e::catalogue, 0, 48, {}, true }, { "ms", type_e::integer, 0, 3600000, {}, true } } });
        t.push_back({ kind_e::experience, "device_facts", {
                                                            { "gpu_vendor", type_e::catalogue, 0, 24 },
                                                            { "gpu_driver", type_e::catalogue, 0, 24 },
                                                            { "os_build", type_e::catalogue, 0, 16 },
                                                            { "displays", type_e::integer, 0, 16 },
                                                            { "refresh_hz", type_e::integer, 0, 1000 },
                                                            { "nic_link", type_e::choice, 0, 0, nic_link_choices },
                                                            { "nic_speed", type_e::choice, 0, 0, nic_speed_choices },
                                                          } });
        // where a stream start was slow (only a start over the stage's bound is reported) and where it failed: fixed stage words, no free text (docs: the stream start stages of the Worker contract)
        t.push_back({ kind_e::experience, "stream_stage", { { "stage", type_e::choice, 0, 0, stream_stage_choices, true }, { "ms", type_e::integer, 0, 3600000 } } });
        t.push_back({ kind_e::experience, "stream_failed", { { "stage", type_e::choice, 0, 0, stream_stage_choices, true }, { "code", type_e::code, 0, 64, {}, true } } });
        // a driver operation that failed (the successes are counted by the feature words): which driver, what was tried, the result code as digits and letters
        t.push_back({ kind_e::error, "driver_event", { { "driver", type_e::choice, 0, 0, driver_choices, true }, { "op", type_e::choice, 0, 0, driver_op_choices, true }, { "result", type_e::choice, 0, 0, ok_failed_choices, true }, { "hresult", type_e::code, 0, 64 } } });
        // the previous run of the host did not end by itself (sent with that run's session id): how it was told apart from a shutdown
        t.push_back({ kind_e::error, "session_abnormal", { { "abnormal_mechanism", type_e::choice, 0, 0, { "hang", "wer_log", "shutdown", "unknown" }, true } } });
        // a feedback report was sent (the counterpart of user_report that survives the daily limit of issues): the category in the Worker's words
        t.push_back({ kind_e::error, "feedback_submit", { { "category", type_e::choice, 0, 0, feedback_category_choices, true } } });
        // the Panel's clipboard agent, one record per run (and one per 24 hours of a long run): how the connection to the host behaved. Counts only, no content.
        t.push_back({ kind_e::experience, "clipboard_health", {
                                                                { "first_connect_ms", type_e::integer, 0, 3600000 },
                                                                { "connected_pct", type_e::choice, 0, 0, clip_conn_choices },
                                                                { "reconnects", type_e::integer, 0, 1000000 },
                                                                { "heartbeat_fail", type_e::integer, 0, 1000000 },
                                                                { "last_error", type_e::choice, 0, 0, clip_error_choices },
                                                                { "apply_ok", type_e::integer, 0, 1000000 },
                                                                { "apply_fail", type_e::integer, 0, 1000000 },
                                                                { "post_ok", type_e::integer, 0, 1000000 },
                                                                { "post_fail", type_e::integer, 0, 1000000 },
                                                                { "runtime_s", type_e::integer, 0, 31536000 },
                                                              } });
        return t;
      }();
      return table;
    }
    // -> the cleaned value, or nullopt when the value does not fit the spec
    std::optional<json>
    clean(const prop_spec_t &spec, const json &value) {
      switch (spec.type) {
        case type_e::boolean:
          if (value.is_boolean()) return value;
          return std::nullopt;
        case type_e::integer: {
          if (!value.is_number()) return std::nullopt;
          const double v = value.get<double>();
          if (!std::isfinite(v)) return std::nullopt;
          return json(static_cast<std::int64_t>(std::llround(std::clamp(v, spec.min, spec.max))));
        }
        case type_e::number: {
          if (!value.is_number()) return std::nullopt;
          const double v = value.get<double>();
          if (!std::isfinite(v)) return std::nullopt;
          return json(std::round(std::clamp(v, spec.min, spec.max) * 10.0) / 10.0);
        }
        case type_e::choice: {
          if (!value.is_string()) return std::nullopt;
          const auto s = value.get<std::string>();
          for (const auto &c : spec.choices)
            if (s == c) return json(s);
          const bool has_other = std::find(spec.choices.begin(), spec.choices.end(), "other"sv) != spec.choices.end();
          if (has_other) return json("other");
          return std::nullopt;
        }
        case type_e::catalogue: {
          if (!value.is_string()) return std::nullopt;
          auto id = catalogue_id(value.get<std::string>(), static_cast<std::size_t>(spec.max));
          if (id.empty()) return std::nullopt;
          return json(std::move(id));
        }
        case type_e::code: {
          // an error code keeps its case (E_DXGI_LOST, 0xC0000005): letters, digits and _ . : + -, at most 64
          if (!value.is_string()) return std::nullopt;
          const auto text = value.get<std::string>();
          if (text.empty() || text.size() > static_cast<std::size_t>(spec.max)) return std::nullopt;
          for (char c : text) {
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || c == '+' || c == '-';
            if (!ok || static_cast<unsigned char>(c) >= 0x80) return std::nullopt;
          }
          return json(text);
        }
        case type_e::attachment: {
          // only the reference the developer panel returns: att-YYYY-MM/<64 hex>.<ext>
          if (!value.is_string()) return std::nullopt;
          static const std::regex ref(R"(^att-\d{4}-\d{2}/[0-9a-f]{64}\.[a-z0-9]{1,8}$)");
          const auto text = value.get<std::string>();
          return std::regex_match(text, ref) ? std::optional<json> { json(text) } : std::nullopt;
        }
        case type_e::log_ref: {
          // only the reference POST /v1/user-log returns: crash-YYYY-MM[-n]/ur-<24 hex>.log.gz
          if (!value.is_string()) return std::nullopt;
          static const std::regex ref(R"(^crash-\d{4}-\d{2}(-\d)?/ur-[0-9a-f]{24}\.log\.gz$)");
          const auto text = value.get<std::string>();
          return std::regex_match(text, ref) ? std::optional<json> { json(text) } : std::nullopt;
        }
        case type_e::shot_ref: {
          // only the reference POST /v1/user-screenshot returns: crash-YYYY-MM[-n]/ur-shot-<24 hex>.<png|jpg|webp>
          if (!value.is_string()) return std::nullopt;
          static const std::regex ref(R"(^crash-\d{4}-\d{2}(-\d)?/ur-shot-[0-9a-f]{24}\.(png|jpg|webp)$)");
          const auto text = value.get<std::string>();
          return std::regex_match(text, ref) ? std::optional<json> { json(text) } : std::nullopt;
        }
        case type_e::reply_key: {
          // the address of the developers' answer to this report: made at random by the page (rk_ and 128 bits), kept by the page on its own computer and written by the server only into the issue of this report
          if (!value.is_string()) return std::nullopt;
          static const std::regex ref(R"(^rk_[0-9a-f]{32}$)");
          const auto text = value.get<std::string>();
          return std::regex_match(text, ref) ? std::optional<json> { json(text) } : std::nullopt;
        }
        case type_e::text: {
          if (!value.is_string()) return std::nullopt;
          return json(scrub_text(value.get<std::string>(), static_cast<std::size_t>(spec.max)));
        }
        case type_e::contact: {
          // what a person left on purpose so the developers can answer: one line, cut, NOT scrubbed (an address they gave is what it is for); it shows only in the issue of that one report
          if (!value.is_string()) return std::nullopt;
          std::string line;
          bool space = false;
          for (const unsigned char c : value.get<std::string>()) {
            if (c < 0x20 || c == 0x7f || c == ' ') {
              space = !line.empty();
              continue;
            }
            if (space) line += ' ';
            space = false;
            line += static_cast<char>(c);
          }
          line = cut_utf8(line, static_cast<std::size_t>(spec.max));
          return line.empty() ? std::nullopt : std::optional<json> { json(line) };
        }
        case type_e::site:
          return value.is_string() && valid_log_site(value.get_ref<const std::string &>()) ? std::optional<json> { value } : std::nullopt;
        case type_e::frame: {
          if (!value.is_string()) return std::nullopt;
          auto frame = clean_frame(value.get<std::string>());
          if (frame.empty()) return std::nullopt;
          return json(std::move(frame));
        }
        case type_e::frames: {
          if (!value.is_array() || value.size() > MAX_FRAMES) return std::nullopt;
          json out = json::array();
          for (const auto &f : value) {
            if (!f.is_string()) return std::nullopt;
            auto frame = clean_frame(f.get<std::string>());
            if (frame.empty()) return std::nullopt;
            out.push_back(std::move(frame));
          }
          return out;
        }
        case type_e::crumbs: {
          if (!value.is_array() || value.size() > MAX_BREADCRUMBS) return std::nullopt;
          static const std::regex crumb(R"(^\d{1,5}\|(?:nav|act|set|stream|dev|state|net)\|[a-z0-9_.:+-]{1,48}$)");
          json out = json::array();
          for (const auto &c : value) {
            if (!c.is_string() || !std::regex_match(c.get<std::string>(), crumb)) return std::nullopt;
            out.push_back(c);
          }
          return out;
        }
        case type_e::log_lines: {
          // The tail of a component's own log, already anonymised by the sender (Panel: log_scrub.rs). Scrubbed here once more with the host's text rules, so a sender that is old or wrong cannot let a path,
          // an address or a key through; at most 40 lines of 200 characters.
          if (!value.is_array() || value.size() > MAX_LOG_LINES) return std::nullopt;
          json out = json::array();
          for (const auto &line : value) {
            if (!line.is_string()) return std::nullopt;
            auto text = scrub_log_message(line.get<std::string>(), MAX_LOG_LINE_CHARS);
            text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }), text.end());
            if (!text.empty()) out.push_back(std::move(text));
          }
          return out;
        }
        case type_e::counts: {
          if (!value.is_object()) return std::nullopt;
          json out = json::object();
          for (const auto &k : health::hist_keys()) {
            const auto found = value.find(std::string { k.key });
            if (found == value.end()) continue;
            if (!found->is_number()) return std::nullopt;
            const double v = found->get<double>();
            if (!std::isfinite(v)) return std::nullopt;
            out[std::string { k.key }] = static_cast<std::int64_t>(std::llround(std::clamp(v, 0.0, 100000.0)));
          }
          return out;
        }
        case type_e::vector8: {
          // exactly 8 non-negative integers; anything else drops the whole prop
          if (!value.is_array() || value.size() != 8) return std::nullopt;
          json row = json::array();
          for (const auto &n : value) {
            if (!n.is_number()) return std::nullopt;
            const double v = n.get<double>();
            if (!std::isfinite(v) || v < 0) return std::nullopt;
            row.push_back(static_cast<std::int64_t>(std::llround(std::clamp(v, 0.0, 10000000.0))));
          }
          return row;
        }
        case type_e::histograms: {
          // only the keys of health::hist_keys(), each exactly BUCKET_COUNT non-negative integers; anything else drops the whole prop
          if (!value.is_object()) return std::nullopt;
          json out = json::object();
          for (const auto &k : health::hist_keys()) {
            const auto found = value.find(std::string { k.key });
            if (found == value.end()) continue;
            if (!found->is_array() || found->size() != health::BUCKET_COUNT) return std::nullopt;
            json row = json::array();
            for (const auto &n : *found) {
              if (!n.is_number()) return std::nullopt;
              const double v = n.get<double>();
              if (!std::isfinite(v)) return std::nullopt;
              row.push_back(static_cast<std::int64_t>(std::llround(std::clamp(v, 0.0, 10000000.0))));
            }
            out[std::string { k.key }] = std::move(row);
          }
          if (out.empty()) return std::nullopt;
          return out;
        }
      }
      return std::nullopt;
    }
  }  // namespace

  std::optional<event_t>
  shape_event(kind_e kind, std::string_view name, const json &props, std::int64_t now_ms) {
    const event_spec_t *spec = nullptr;
    for (const auto &s : specs()) {
      if (s.kind == kind && s.name == name) {
        spec = &s;
        break;
      }
    }
    if (!spec) return std::nullopt;
    if (!props.is_object() && !props.is_null()) return std::nullopt;
    event_t event;
    event.t = now_ms;
    event.kind = kind;
    event.name = std::string { name };
    for (const auto &p : spec->props) {
      const auto found = props.is_object() ? props.find(std::string { p.key }) : props.end();
      if (props.is_object() && found != props.end()) {
        if (auto cleaned = clean(p, *found)) {
          event.props[std::string { p.key }] = std::move(*cleaned);
          continue;
        }
      }
      if (p.required) return std::nullopt;
    }
    return event;
  }

  // ---------------------------------------------------------------------------------------------------------------------
  // settings that are reported when they change

  namespace {
    struct setting_spec_t {
      std::string_view key;
      std::vector<std::string_view> values;  // empty: a number bucket
      std::int64_t step = 0;
      std::vector<std::int64_t> edges {};  // when set: a range bucket "lo-hi" between these edges ("200000+" above the last)
    };

    const std::vector<setting_spec_t> &
    settings() {
      static const std::vector<setting_spec_t> table {
        { "encoder", { "nvenc", "quicksync", "amdvce", "vaapi", "videotoolbox", "software", "" } },
        { "capture", { "wgc", "ddx", "dxgi", "nvfbc", "kms", "x11", "wlr", "" } },
        { "hevc_mode", { "0", "1", "2", "3" } },
        { "av1_mode", { "0", "1", "2", "3" } },
        { "hdr_prep", { "0", "1", "2", "auto", "no_operation", "yes", "no" } },
        { "upnp", { "enabled", "disabled", "true", "false" } },
        { "address_family", { "ipv4", "both" } },
        { "origin_web_ui_allowed", { "pc", "lan", "wan" } },
        { "lan_encryption_mode", { "0", "1", "2" } },
        { "wan_encryption_mode", { "0", "1", "2" } },
        { "min_log_level", { "0", "1", "2", "3", "4", "5", "6", "verbose", "debug", "info", "warning", "error", "fatal", "none" } },
        { "gamepad", { "auto", "x360", "ds4", "ds5", "switch", "xone" } },
        { "stop_on_last_video_session", { "enabled", "disabled", "true", "false" } },
        { "mat_live", { "enabled", "disabled", "true", "false" } },
        { "vdd_keep_enabled", { "enabled", "disabled", "true", "false" } },
        { "vdd_reuse", { "enabled", "disabled", "true", "false" } },
        { "nvenc_preset", { "1", "2", "3", "4", "5", "6", "7" } },
        { "sw_preset", { "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow" } },
        { "minimum_fps_target", {}, 10 },
        { "fec_percentage", {}, 5 },
        { "qp", {}, 5 },
        { "min_threads", {}, 1 },
        { "max_bitrate", {}, 0, { 0, 8000, 15000, 30000, 60000, 100000, 200000 } },
        { "sw_tune", { "hq", "film", "animation", "grain", "stillimage", "psnr", "ssim", "fastdecode", "zerolatency" } },
        { "nvenc_twopass", { "disabled", "quarter_res", "full_res" } },
        { "nvenc_spatial_aq", { "enabled", "disabled", "true", "false" } },
        { "locale", { "en", "zh", "zh-CN", "zh-TW", "ja", "ko", "de", "fr", "es", "pt", "ru", "it", "pl", "tr", "uk", "vi", "id", "th", "ar", "hi", "cs", "sv", "bg", "hu", "nl", "en_US", "en_GB", "zh_CN", "zh_TW", "pt_BR", "es_MX", "fr_FR" } },
      };
      return table;
    }
  }  // namespace

  bool
  setting_key_allowed(std::string_view key) {
    for (const auto &s : settings())
      if (s.key == key) return true;
    return false;
  }

  const std::vector<std::string_view> &
  setting_keys() {
    static const std::vector<std::string_view> keys = [] {
      std::vector<std::string_view> k;
      for (const auto &s : settings()) k.push_back(s.key);
      return k;
    }();
    return keys;
  }

  std::int64_t
  round_to_step(double value, std::int64_t step) {
    if (step <= 0) return static_cast<std::int64_t>(std::llround(value));
    return static_cast<std::int64_t>(std::floor(value / static_cast<double>(step))) * step;
  }

  std::string
  setting_value_bucket(std::string_view key, std::string_view value) {
    for (const auto &s : settings()) {
      if (s.key != key) continue;
      if (s.values.empty()) {
        // a number: its lower step, or the range between two edges
        try {
          std::size_t used = 0;
          const auto n = std::stod(std::string { value }, &used);
          if (used == 0 || !std::isfinite(n)) return "other";
          if (!s.edges.empty()) {
            if (n < 0) return "other";
            for (std::size_t i = 0; i + 1 < s.edges.size(); ++i)
              if (n < static_cast<double>(s.edges[i + 1])) return std::to_string(s.edges[i]) + "-" + std::to_string(s.edges[i + 1]);
            return std::to_string(s.edges.back()) + "+";
          }
          return std::to_string(round_to_step(n, s.step));
        }
        catch (...) {
          return "other";
        }
      }
      std::string lower { value };
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lower.empty()) return std::find(s.values.begin(), s.values.end(), ""sv) != s.values.end() ? "auto" : "other";
      for (const auto &v : s.values) {
        std::string vl { v };
        std::transform(vl.begin(), vl.end(), vl.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (vl == lower) return lower;
      }
      return "other";
    }
    return {};
  }

  // ---------------------------------------------------------------------------------------------------------------------
  // envelope

  json
  event_to_json(const event_t &event) {
    json j {
      { "t", event.t },
      { "kind", std::string { kind_name(event.kind) } },
      { "name", event.name },
      { "props", event.props },
    };
    if (event.count > 1) j["count"] = event.count;
    return j;
  }

  std::optional<event_t>
  event_from_json(const json &j) {
    if (!j.is_object()) return std::nullopt;
    const auto kind = j.contains("kind") && j["kind"].is_string() ? parse_kind(j["kind"].get<std::string>()) : std::nullopt;
    if (!kind || !j.contains("name") || !j["name"].is_string() || !j.contains("t") || !j["t"].is_number_integer()) return std::nullopt;
    event_t e;
    e.t = j["t"].get<std::int64_t>();
    e.kind = *kind;
    e.name = j["name"].get<std::string>();
    e.props = j.contains("props") && j["props"].is_object() ? j["props"] : json::object();
    return e;
  }

  namespace {
    bool
    all_of_chars(std::string_view text, bool (*ok)(unsigned char)) {
      for (const char c : text)
        if (!ok(static_cast<unsigned char>(c))) return false;
      return true;
    }
  }  // namespace

  std::string
  safe_version(std::string_view version) {
    // letters, digits and . _ + - space; a byte of a multi-byte letter (the product's own suffix is Chinese)
    // a version starts with a digit, or with a single "v" and then a digit: the release tag the host carries is "v2026.1114.173055.杂鱼" (the same string as in a crash report);
    // a word, a name or a path is not one
    const auto first_digit = version.size() > 1 && (version[0] == 'v' || version[0] == 'V') ? 1U : 0U;
    if (version.empty() || version.size() > 64 || !std::isdigit(static_cast<unsigned char>(version[first_digit]))) return {};
    const auto ok = [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || c == ' ' || c >= 0x80; };
    return all_of_chars(version, ok) ? std::string { version } : std::string {};
  }

  std::string
  safe_os_version(std::string_view text) {
    if (text.size() > 48) text = text.substr(0, 48);
    const auto ok = [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-' || c == ' '; };
    return all_of_chars(text, ok) ? std::string { text } : std::string {};
  }

  std::string
  safe_locale(std::string_view text) {
    // "zh", "zh-CN", "zh_CN", "pt-BR", "en-US-x": a language tag, never a typed word
    static const std::regex tag(R"(^[A-Za-z]{2,3}([-_][A-Za-z0-9]{2,8}){0,2}$)");
    const std::string s { text };
    return std::regex_match(s, tag) ? s : std::string {};
  }

  json
  envelope_fields(const context_t &c, std::string_view product, const std::string &version) {
    auto fields = json {
      { "v", WIRE_VERSION },
      { "install_id", c.install_id },
      { "session_id", c.session_id },
      { "product", std::string { product } },
      { "version", safe_version(version) },
      { "channel", c.channel },
      { "os", c.os },
      { "os_version", safe_os_version(c.os_version) },
      { "arch", c.arch },
      { "gpu_vendor", c.gpu_vendor },
      { "locale", safe_locale(c.locale) },
    };
    // a whole number when it is one (10, not 10.0), at most the weight of the lowest sample rate
    if (c.sample_weight > 1.0) {
      const auto weight = std::min(c.sample_weight, 100.0);
      fields["sample_weight"] = weight == std::floor(weight) ? json(static_cast<std::int64_t>(weight)) : json(std::round(weight * 100.0) / 100.0);
    }
    return fields;
  }

  namespace {
    json
    envelope_head(const context_t &c, std::string_view product, const std::string &version) {
      auto head = envelope_fields(c, product, version);
      head["events"] = json::array();
      return head;
    }
  }  // namespace

  std::vector<json>
  build_envelopes(const context_t &context, std::string_view product, const std::vector<event_t> &events) {
    std::vector<std::vector<std::size_t>> members;
    return build_envelopes(context, product, events, members);
  }

  std::vector<json>
  build_envelopes(const context_t &context, std::string_view product, const std::vector<event_t> &events, std::vector<std::vector<std::size_t>> &members) {
    std::vector<json> out;
    // a product version on an event (panel and web run their own version) starts its own envelope: the version is a field of the envelope
    std::vector<std::string> versions;
    for (const auto &e : events) {
      const auto &v = e.version.empty() ? context.version : e.version;
      if (std::find(versions.begin(), versions.end(), v) == versions.end()) versions.push_back(v);
    }
    for (const auto &version : versions) {
      json current = envelope_head(context, product, version);
      std::size_t bytes = current.dump().size();
      std::vector<std::size_t> current_members;
      const auto flush = [&] {
        if (!current["events"].empty()) {
          out.push_back(std::move(current));
          members.push_back(std::move(current_members));
        }
        current = envelope_head(context, product, version);
        bytes = current.dump().size();
        current_members.clear();
      };
      for (std::size_t at = 0; at < events.size(); ++at) {
        const auto &e = events[at];
        if ((e.version.empty() ? context.version : e.version) != version) continue;
        auto j = event_to_json(e);
        const auto size = j.dump().size() + 1;
        if (current["events"].size() >= MAX_BATCH_EVENTS || bytes + size > MAX_BATCH_BYTES) flush();
        current["events"].push_back(std::move(j));
        current_members.push_back(at);
        bytes += size;
      }
      flush();
    }
    return out;
  }

  std::vector<event_t>
  aggregate_events(const std::vector<event_t> &events, std::vector<std::vector<std::size_t>> &origin) {
    struct acc_t {
      std::size_t out_at;
      std::map<std::string, double> sum;
      std::map<std::string, std::uint64_t> seen;
      std::set<std::string> fractional;
    };
    std::vector<event_t> out;
    origin.clear();
    std::map<std::string, acc_t> groups;
    for (std::size_t i = 0; i < events.size(); ++i) {
      const auto &e = events[i];
      if (!high_volume_event(e.name) || !e.report_id.empty() || !e.report_install.empty() || !e.props.is_object()) {
        out.push_back(e);
        origin.push_back({ i });
        continue;
      }
      const bool additive = e.name == "page_vitals" || e.name == "api_latency";
      json dims = json::object();
      for (auto it = e.props.begin(); it != e.props.end(); ++it)
        if (!it.value().is_number() && !(additive && it.value().is_array())) dims[it.key()] = it.value();
      const auto key = e.product + '\0' + e.name + '\0' + std::string { kind_name(e.kind) } + '\0' + e.version + '\0' + std::to_string(e.priority) + '\0' + dims.dump();
      auto found = groups.find(key);
      if (found == groups.end()) {
        found = groups.emplace(key, acc_t { out.size(), {}, {}, {} }).first;
        out.push_back(e);
        origin.push_back({});
        out.back().count = 0;
      }
      auto &a = found->second;
      auto &target = out[a.out_at];
      origin[a.out_at].push_back(i);
      ++target.count;
      target.t = std::max(target.t, e.t);
      for (auto it = e.props.begin(); it != e.props.end(); ++it) {
        const auto &v = it.value();
        if (v.is_number()) {
          a.sum[it.key()] += v.get<double>();
          ++a.seen[it.key()];
          if (!v.is_number_integer()) a.fractional.insert(it.key());
        }
        else if (additive && v.is_array() && origin[a.out_at].size() > 1) {
          auto &dst = target.props[it.key()];
          if (!dst.is_array())
            dst = v;  // an optional bucket vector the first record did not carry
          else if (dst.size() == v.size())
            for (std::size_t k = 0; k < v.size(); ++k)
              if (v[k].is_number() && dst[k].is_number()) dst[k] = dst[k].get<double>() + v[k].get<double>();
        }
      }
    }
    for (auto &[key, a] : groups) {
      auto &target = out[a.out_at];
      const bool additive = target.name == "page_vitals" || target.name == "api_latency";
      for (const auto &[prop, total] : a.sum) {
        const double value = additive ? total : total / static_cast<double>(a.seen[prop]);
        if (a.fractional.count(prop))
          target.props[prop] = value;
        else
          target.props[prop] = static_cast<std::int64_t>(std::llround(value));
      }
    }
    return out;
  }

  // ---------------------------------------------------------------------------------------------------------------------
  // numbers

  double
  percentile(std::vector<double> samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    p = std::clamp(p, 0.0, 100.0);
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(samples.size())));
    return samples[std::min(samples.size() - 1, rank == 0 ? 0 : rank - 1)];
  }

  histogram_t::histogram_t(double max_value, std::size_t buckets):
      max_(max_value), width_(max_value / static_cast<double>(std::max<std::size_t>(1, buckets))), bins_(std::max<std::size_t>(1, buckets) + 1, 0) {}

  void
  histogram_t::add(double value) {
    if (!std::isfinite(value) || value < 0) return;
    const auto index = value >= max_ ? bins_.size() - 1 : static_cast<std::size_t>(value / width_);
    ++bins_[std::min(index, bins_.size() - 1)];
    ++count_;
  }

  double
  histogram_t::percentile(double p) const {
    if (count_ == 0) return 0;
    p = std::clamp(p, 0.0, 100.0);
    const auto target = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(count_))));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < bins_.size(); ++i) {
      seen += bins_[i];
      if (seen >= target) return std::min(max_, static_cast<double>(i + 1) * width_);
    }
    return max_;
  }

  void
  histogram_t::clear() {
    std::fill(bins_.begin(), bins_.end(), 0);
    count_ = 0;
  }

}  // namespace telemetry
