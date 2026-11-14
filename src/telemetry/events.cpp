/**
 * @file src/telemetry/events.cpp
 * @brief Telemetry v1 event shapes, scrubbing and the batch envelope.
 */
#include "events.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <functional>
#include <regex>

using json = nlohmann::json;
using namespace std::literals;

namespace telemetry {

  // ---------------------------------------------------------------------------------------------------------------------
  // kinds, products

  std::string_view
  kind_name(kind_e kind) {
    switch (kind) {
      case kind_e::usage: return "usage"sv;
      case kind_e::error: return "error"sv;
      case kind_e::experience: return "experience"sv;
    }
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

  bool
  known_product(std::string_view product) {
    return product == "host"sv || product == "panel"sv || product == "web"sv || product == "sparxie-launch"sv;
  }

  bool
  client_may_send(std::string_view product, kind_e kind, std::string_view name) {
    if (product != "panel"sv && product != "web"sv && product != "sparxie-launch"sv) return false;
    if (kind == kind_e::usage) return name == "app_start"sv || name == "app_exit"sv || name == "feature_used"sv || name == "setting_changed"sv;
    // "user_report" is a report the person wrote in the error dialog; it is kind error and follows the error-report choice like every error event
    if (kind == kind_e::error) return name == "error"sv || name == "user_report"sv;
    return name == "panel_page"sv || name == "startup"sv;
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
        const std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 1;
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
      // eight groups, or a compressed form with one "::"; only hex groups of at most four digits; at least one decimal digit (so std::vector style text is not an address)
      if (s.find(":::") != std::string::npos) return false;
      bool digit = false;
      for (char c : s) digit = digit || (c >= '0' && c <= '9');
      if (!digit) return false;
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
    // A log line names things the person chose (a device, an application, a game) between quotes or brackets: what stands between them is not part of the message.
    std::string text { input.substr(0, max * 4) };
    // a double-quoted, bracketed or braced span; a single-quoted one only when the quote opens a word (not an apostrophe: "Couldn't")
    static const std::regex quoted(R"(("[^"]*")|(\[[^\]]*\])|(\{[^}]*\})|((^|[\s(=:,])'[^\n]*?'(?=$|[\s).,;:!?\]])))");
    // keep a bracket that only holds numbers or a short upper-case tag (a code, a count), drop the rest
    text = replace_fn(text, quoted, [](const std::smatch &m) {
      const auto s = m.str(0);
      if (m[4].matched) return m.str(5) + "<…>";
      const auto inner = s.substr(1, s.size() - 2);
      static const std::regex plain(R"(^[A-Z0-9_ .:=,-]{0,24}$)");
      return (s.front() == '[' && std::regex_match(inner, plain)) ? s : std::string { "<…>" };
    });
    return scrub_text(text, max);
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
    enum class type_e { boolean, integer, number, choice, catalogue, code, attachment, text };

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
    const std::vector<std::string_view> update_source_choices { "mirror", "github", "other" };

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

    const std::vector<event_spec_t> &
    specs() {
      static const std::vector<event_spec_t> table = [] {
        std::vector<event_spec_t> t;
        t.push_back({ kind_e::usage, "app_start", { { "cold", type_e::boolean }, { "start_ms", type_e::integer, 0, 600000 }, { "update_source", type_e::choice, 0, 0, update_source_choices } } });
        t.push_back({ kind_e::usage, "app_exit", { { "uptime_s", type_e::integer, 0, 31536000 } } });
        t.push_back({ kind_e::usage, "stream_start", stream_dims() });
        auto end = stream_dims();
        end.push_back({ "duration_s", type_e::integer, 0, 604800 });
        end.push_back({ "result", type_e::choice, 0, 0, result_choices, true });
        t.push_back({ kind_e::usage, "stream_end", end });
        t.push_back({ kind_e::usage, "feature_used", { { "feature", type_e::catalogue, 0, 48, {}, true } } });
        t.push_back({ kind_e::usage, "setting_changed", { { "key", type_e::catalogue, 0, 48, {}, true }, { "value", type_e::catalogue, 0, 32, {}, true } } });
        t.push_back({ kind_e::error, "error", {
                                                { "component", type_e::catalogue, 0, 32, {}, true },
                                                { "code", type_e::code, 0, 64 },
                                                { "severity", type_e::choice, 0, 0, severity_choices },
                                                { "count", type_e::integer, 1, 1000000 },
                                                // no message: a line of the log or a text a page caught can carry what a person chose (a device, a game, a path). The code names the error; the one place for typed text is user_report.
                                              } });
        // what the person wrote in the error dialog (spec "Worker additions"): a category, the component and page as tokens, an error code, the reference of an uploaded attachment and one scrubbed message of at most 2000 characters; no contact field
        t.push_back({ kind_e::error, "user_report", {
                                                      { "category", type_e::choice, 0, 0, report_category_choices },
                                                      { "component", type_e::catalogue, 0, 32, {}, true },
                                                      { "page", type_e::catalogue, 0, 48 },
                                                      { "error_code", type_e::code, 0, 64 },
                                                      { "attachment", type_e::attachment },
                                                      { "message", type_e::text, 0, static_cast<double>(MAX_USER_REPORT_CHARS), {}, true },
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
                                                            } });
        t.push_back({ kind_e::experience, "startup", { { "host_start_ms", type_e::integer, 0, 3600000 }, { "first_frame_ms", type_e::integer, 0, 3600000 } } });
        t.push_back({ kind_e::experience, "panel_page", { { "page", type_e::catalogue, 0, 40, {}, true }, { "time_to_content_ms", type_e::integer, 0, 600000, {}, true } } });
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
        case type_e::text: {
          if (!value.is_string()) return std::nullopt;
          return json(scrub_text(value.get<std::string>(), static_cast<std::size_t>(spec.max)));
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
    // a version starts with a digit ("2026.1112.0.杂鱼"); a word, a name or a path is not one
    if (version.empty() || version.size() > 64 || !std::isdigit(static_cast<unsigned char>(version[0]))) return {};
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
    return json {
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
      const auto flush = [&] {
        if (!current["events"].empty()) out.push_back(std::move(current));
        current = envelope_head(context, product, version);
        bytes = current.dump().size();
      };
      for (const auto &e : events) {
        if ((e.version.empty() ? context.version : e.version) != version) continue;
        auto j = event_to_json(e);
        const auto size = j.dump().size() + 1;
        if (current["events"].size() >= MAX_BATCH_EVENTS || bytes + size > MAX_BATCH_BYTES) flush();
        current["events"].push_back(std::move(j));
        bytes += size;
      }
      flush();
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
