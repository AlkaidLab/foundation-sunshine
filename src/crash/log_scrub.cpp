/**
 * @file src/crash/log_scrub.cpp
 * @brief Anonymisation of a whole session log; a port of the reference implementation, driven by the shared vectors (see log_scrub.h).
 */
#include "log_scrub.h"

#include <algorithm>
#include <boost/regex.hpp>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace crash::logscrub {
  namespace {
    using boost::regex;
    constexpr auto I = boost::regex::perl | boost::regex::icase;
    constexpr auto S = boost::regex::perl;

    // ---- UTF-8 -------------------------------------------------------------------------------------------------------------------
    /// Length of the valid UTF-8 sequence at `p` (0 when invalid), and its code point.
    std::size_t
    utf8_at(std::string_view s, std::size_t p, std::uint32_t &cp) {
      const auto b0 = static_cast<unsigned char>(s[p]);
      auto cont = [&](std::size_t i) { return p + i < s.size() && (static_cast<unsigned char>(s[p + i]) & 0xC0) == 0x80; };
      if (b0 < 0x80) {
        cp = b0;
        return 1;
      }
      if (b0 >= 0xC2 && b0 <= 0xDF && cont(1)) {
        cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[p + 1]) & 0x3Fu);
        return 2;
      }
      if (b0 >= 0xE0 && b0 <= 0xEF && cont(1) && cont(2)) {
        cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[p + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[p + 2]) & 0x3Fu);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        return 3;
      }
      if (b0 >= 0xF0 && b0 <= 0xF4 && cont(1) && cont(2) && cont(3)) {
        cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[p + 1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(s[p + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[p + 3]) & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        return 4;
      }
      return 0;
    }

    bool
    is_dropped_control(std::uint32_t cp) {
      return cp <= 0x08 || cp == 0x0B || cp == 0x0C || (cp >= 0x0E && cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029 || cp == 0xFEFF || (cp >= 0x202A && cp <= 0x202E) ||
             (cp >= 0x2066 && cp <= 0x2069);
    }

    // ---- placeholders ------------------------------------------------------------------------------------------------------------
    struct labels_t {
      std::map<std::string, std::map<std::string, int>> maps;
      std::string
      of(const std::string &kind, std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        auto &m = maps[kind];
        auto it = m.find(value);
        if (it == m.end()) it = m.emplace(value, (int) m.size() + 1).first;
        return "<" + kind + "#" + std::to_string(it->second) + ">";
      }
    };

    /// text.replace(re, fn) for a callback taking the match
    template <class F>
    std::string
    replace_all(const std::string &text, const regex &re, F &&fn) {
      std::string out;
      out.reserve(text.size());
      auto last = text.cbegin();
      for (boost::sregex_iterator it(text.cbegin(), text.cend(), re), end; it != end; ++it) {
        const auto &m = *it;
        if (m.length(0) == 0) continue;
        out.append(last, m[0].first);
        out += fn(m);
        last = m[0].second;
      }
      out.append(last, text.cend());
      return out;
    }

    // ---- the rules (same order and patterns as the reference implementation) ---------------------------------------------------------
    const char *const SECRET_KEY = R"((?:[\w.-]*(?:token|secret|passwd|password|passphrase|pwd|apikey|api[_-]?key|private[_-]?key|credential|cookie|authorization|auth|passcode|cdk|salt|nonce)[\w.-]*|(?:[\w.-]*[_.-])?pin|pairing[_-]?pin))";
    const char *const SERIAL_KEY = R"((?:[\w.-]*serial(?:[_ -]?(?:number|no))?|sn|s/n|[\w.-]*(?:device|hardware|machine|instance|unique|monitor|display)[_ -]?id|udid|imei|edid[_ -]?(?:serial|hash)))";
    const char *const ACCOUNT_KEY = R"((?:[\w.-]*(?:account|nickname|nick[_ -]?name|player[_ -]?name|role[_ -]?name|user[_ -]?name|username|login|owner|steam[_ -]?id|uid|user[_ -]?id|profile[_ -]?name)[\w.-]*))";
    const char *const SSID_KEY = R"((?:ssid|essid|wi-?fi(?:[_ -]?(?:name|network))?|wlan[_ -]?name|network[_ -]?name|profile))";
    const char *const HOST_KEY = R"((?:host[_ -]?name|computer[_ -]?name|machine[_ -]?name|device[_ -]?name|node[_ -]?name|netbios[_ -]?name|pc[_ -]?name))";

    regex
    kv(const std::string &key) {
      return regex(R"(\b()" + key + R"()(["']?\s*[:=]\s*)("[^"\r\n]*"|'[^'\r\n]*'|[^\s,;&"'}\]]+))", I);
    }

    struct rules_t {
      regex re_secret = kv(SECRET_KEY), re_serial = kv(SERIAL_KEY), re_account = kv(ACCOUNT_KEY), re_ssid = kv(SSID_KEY), re_host = kv(HOST_KEY);
      regex harmless { R"(^(?:["']?(?:|true|false|null|none|nil|empty|n/a|unknown|default|disabled|enabled|on|off|yes|no|-?\d{1,6}|<[^>]*>)["']?)$)", I };
      regex pin_digits { R"(\d)", S };
      regex pin_key { R"(pin|passcode)", I };
      // a number under one of these keys is a credential (password=123456), unless the key says it counts or measures something (token_count, max_password_length)
      regex secret_num_key { R"(pass|pwd|secret|token|credential|cookie|authorization|pin|salt|nonce)", I };
      regex metric_key { R"((?:^|[_.-])(?:count|len|length|size|timeout|ttl|ms|secs?|seconds|max|min|limit|interval|retries|retry|attempts|version|port|enabled|expires?|age|bytes|bits|rate|percent)(?:$|[_.-]))", I };
      regex number_value { R"(^["']?-?\d{1,6}["']?$)", S };
      regex cookie_line { R"((\b(?:set-)?cookie2?\b\s*:\s*)[^\r\n]+)", I };
      regex login_key { R"(^(?:[\w.-]*(?:user[_ -]?name|username|login|owner))$)", I };
      regex query { R"(([?&][A-Za-z0-9_.-]+)=[^&\s#"']*)", S };
      regex url_cred { R"(\bhttps?://[^\s/@"']+@)", I };
      regex jwt { R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{4,}\b)", S };
      regex bearer { R"(\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{8,})", S };
      regex known_tokens { R"(\b(?:sk-[A-Za-z0-9_-]{12,}|gh[pousr]_[A-Za-z0-9]{20,}|xox[abp]-[A-Za-z0-9-]{10,}|AKIA[0-9A-Z]{16})\b)", S };
      regex pin_words { R"(\b(PIN|passcode|pairing code)\b(\s+(?:is\s+)?)(\d{4,8})\b)", I };
      regex device_instance { R"(\b((?:PCI|USB|HID|ACPI|SWD|ROOT|DISPLAY|MONITOR|BTHENUM|BTHLE|HDAUDIO|MMDEVAPI|SCSI|STORAGE|IDE|NVME)[\\#][^\\#\s"']+)[\\#](?!<)([^\\#\s"']+))", I };
      regex device_path { R"(\b(\\\\\?\\)(?:[A-Za-z]+#)[^\s"']+)", S };
      regex guid { R"(\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b)", I };
      regex sid { R"(\bS-1-\d+(?:-\d+){2,}\b)", S };
      regex steam { R"(\b7656119\d{10}\b)", S };
      regex wifi_quoted { R"(\b(SSID|Wi-?Fi)\b(\s+)("[^"\r\n]{1,64}"|'[^'\r\n]{1,64}'))", I };
      regex win_user { R"(\b([A-Za-z]):([\\/])Users\2(?!(?:Public|Default|All Users|<)\b)[^\\/\s"'<>|]+)", I };
      regex posix_user { R"(/(Users|home)/(?!(?:Shared|Guest|<)\b)[^/\s"'<>|]+)", S };
      regex unc { R"(\\\\(?!\?\\)(?:<host#\d+>|[^\s\\"'<>|]+)\\[^\s\\"'<>|]+)", S };
      regex email { R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})", S };
      regex local_host { R"(\b[A-Za-z0-9-]+\.(?:local|lan|home|internal|localdomain|home\.arpa)\b)", I };
      regex mac { R"((?<![\w:-])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![\w:-]))", I };
      regex ip_any {
        R"((?<![\w:.])(?:(?:[0-9a-f]{1,4}:){7}[0-9a-f]{1,4}|(?:[0-9a-f]{1,4}(?::[0-9a-f]{1,4}){0,6})?::(?:[0-9a-f]{1,4}(?::[0-9a-f]{1,4}){0,6})?)(?![\w:])|(?<![\w.])(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}(?![\w.]*\d))",
        I
      };
      regex versionish { R"((?:version|ver\.?|v|driver|build|dll|sdk|api|firmware|fw)[\s:=/_-]*$)", I };
      regex clock { R"(^\d{1,2}:\d{2}(?::\d{2})?$)", S };
      regex opaque { R"(\b[A-Za-z0-9_-]{32,}\b)", S };
      regex hex40 { R"(^[0-9a-f]{40}$)", I };
      regex letters { R"(^[A-Za-z]+$)", S };
      regex commit_ctx { R"((?:commit|sha1?|hash|build|rev|revision)[\s:="']*$)", I };
      regex pem_begin { R"(-----BEGIN [A-Z ]*(?:PRIVATE KEY|CERTIFICATE|PUBLIC KEY)[A-Z ]*-----)", S };
      regex pem_end { R"(-----END [A-Z ]+-----)", S };
      regex path_win { R"(\b[A-Za-z]:[\\/]Users[\\/]([^\\/\s"'<>|:*?]+(?: [^\\/\s"'<>|:*?]+)*?)(?=[\\/]))", S };
      regex path_mac { R"(/Users/([^/\s"'<>|]+))", S };
      regex path_linux { R"(/home/([^/\s"'<>|]+))", S };
      regex unc_host { R"(\\\\([A-Za-z0-9_-]{2,63})(?=\\))", S };
      regex win_host { R"(\b((?:DESKTOP|LAPTOP|WIN|PC|NOTEBOOK)-[A-Z0-9]{5,13})\b)", S };
      regex quotes { R"(^["']|["']$)", S };
      // a learned name that is itself a key word (username=password) stays where a key stands, so the key rules still see it
      regex name_is_key { std::string("^(?:") + SECRET_KEY + "|" + SERIAL_KEY + "|" + ACCOUNT_KEY + "|" + SSID_KEY + "|" + HOST_KEY + ")$", I };
      regex sep_follows { R"(\s*[:=])", S };
    };

    const rules_t &
    rules() {
      static const rules_t r;
      return r;
    }

    const std::set<std::string> PUBLIC_NAMES = { "public", "default", "default user", "all users", "defaultuser0", "system", "system32", "localservice", "networkservice",
                                                 "administrator", "administrators", "guest", "shared", "user", "users", "root", "appdata", "programdata", "windows" };
    const std::set<std::string> KEEP_IPV4 = { "0.0.0.0", "127.0.0.1", "255.255.255.255", "255.255.255.0" };

    std::string
    lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
      return s;
    }
    std::string
    trim(const std::string &s) {
      const auto a = s.find_first_not_of(" \t\r\n");
      if (a == std::string::npos) return {};
      return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }
    bool
    is_harmless(const std::string &v) {
      return boost::regex_match(trim(v), rules().harmless);
    }
    std::string
    unquote(const std::string &v) {
      return boost::regex_replace(v, rules().quotes, "");
    }
    std::string
    escape_re(const std::string &s) {
      std::string out;
      for (char c : s) {
        if (std::string_view(".*+?^${}()|[]\\").find(c) != std::string_view::npos) out += '\\';
        out += c;
      }
      return out;
    }

    /// Names the log itself reveals: the user segment of profile paths and the value of user / computer name keys.
    void
    learn_names(const std::string &text, std::vector<std::string> &users, std::vector<std::string> &hosts) {
      const auto &r = rules();
      std::set<std::string> u, h;
      for (const regex *re : { &r.path_win, &r.path_mac, &r.path_linux })
        for (boost::sregex_iterator it(text.cbegin(), text.cend(), *re), end; it != end; ++it) u.insert((*it)[1].str());
      for (boost::sregex_iterator it(text.cbegin(), text.cend(), r.re_account), end; it != end; ++it)
        if (boost::regex_match((*it)[1].str(), r.login_key)) u.insert(unquote((*it)[3].str()));
      for (boost::sregex_iterator it(text.cbegin(), text.cend(), r.re_host), end; it != end; ++it) h.insert(unquote((*it)[3].str()));
      for (boost::sregex_iterator it(text.cbegin(), text.cend(), r.unc_host), end; it != end; ++it) h.insert((*it)[1].str());
      for (boost::sregex_iterator it(text.cbegin(), text.cend(), r.win_host), end; it != end; ++it) h.insert((*it)[1].str());
      auto clean = [&](const std::set<std::string> &in, std::vector<std::string> &out) {
        for (const auto &raw : in) {
          const auto x = trim(raw);
          if (x.size() >= 3 && x.size() <= 64 && !PUBLIC_NAMES.count(lower(x)) && !is_harmless(x) && !(x.front() == '<' && x.back() == '>')) out.push_back(x);
        }
      };
      clean(u, users);
      clean(h, hosts);
    }

    /// True when the value of a rule left a quote that cannot be balanced: the value was opened by a quote (or cut short by one) and the rest of the line holds an odd number of that
    /// quote character. What follows such a quote cannot be told from the rest of the value (`'Players' choice - Budget.xlsx'`, `Bob' phone`), so it goes with it.
    bool
    quote_unbalanced(const std::string &value, std::string::const_iterator rest, std::string::const_iterator end) {
      char q = 0;
      if (!value.empty() && (value.front() == '\'' || value.front() == '"')) q = value.front();
      else if (rest != end && (*rest == '\'' || *rest == '"')) q = *rest;
      if (!q) return false;
      return std::count(rest, end, q) % 2 == 1;
    }

    /// key [:=] value, where `fmt` writes the replacement unless the value is harmless (or `keep` says it must be hidden anyway). A quote that cannot be balanced drops the rest of the line.
    template <class Keep, class Fmt>
    std::string
    kv_sub(const std::string &line, const regex &re, Keep &&must_hide, Fmt &&fmt) {
      std::string out;
      out.reserve(line.size());
      auto last = line.cbegin();
      for (boost::sregex_iterator it(line.cbegin(), line.cend(), re), end; it != end; ++it) {
        const auto &m = *it;
        if (m.length(0) == 0) continue;
        const auto v = m[3].str();
        if (is_harmless(v) && !must_hide(m[1].str(), v)) {
          // a value that is one of our own placeholders (a learned name was replaced first) has the same unbalanced quote behind it
          if (v.size() > 1 && v.front() == '<' && v.back() == '>' && quote_unbalanced(v, m[0].second, line.cend())) {
            out.append(last, m[0].second);
            return out;
          }
          continue;
        }
        out.append(last, m[0].first);
        out += fmt(m[1].str(), m[2].str());
        last = m[0].second;
        if (quote_unbalanced(v, last, line.cend())) return out;
      }
      out.append(last, line.cend());
      return out;
    }

    std::string
    scrub_line(std::string line, labels_t &labels) {
      const auto &r = rules();
      auto never = [](const std::string &, const std::string &) { return false; };
      line = replace_all(line, r.query, [](const boost::smatch &m) { return m[1].str() + "=<hidden>"; });
      line = replace_all(line, r.url_cred, [](const boost::smatch &m) { return m[0].str().substr(0, m[0].str().find("//") + 2) + "<hidden>@"; });
      line = replace_all(line, r.jwt, [](const boost::smatch &) { return std::string("<jwt>"); });
      line = replace_all(line, r.bearer, [](const boost::smatch &m) { return m[1].str() + " <hidden>"; });
      line = replace_all(line, r.known_tokens, [](const boost::smatch &) { return std::string("<hidden>"); });
      line = replace_all(line, r.cookie_line, [](const boost::smatch &m) { return m[1].str() + "<hidden>"; });  // a Cookie header is one secret, whatever it holds
      line = kv_sub(line, r.re_secret, [&](const std::string &k, const std::string &v) {
          if (boost::regex_search(k, r.pin_key) && boost::regex_search(v, r.pin_digits)) return true;
          return boost::regex_match(trim(v), r.number_value) && boost::regex_search(k, r.secret_num_key) && !boost::regex_search(k, r.metric_key);
        },
        [](const std::string &k, const std::string &sep) { return k + sep + "<hidden>"; });
      line = replace_all(line, r.pin_words, [](const boost::smatch &m) { return m[1].str() + m[2].str() + "<hidden>"; });
      line = kv_sub(line, r.re_serial, never, [](const std::string &k, const std::string &sep) { return k + sep + "<serial>"; });
      line = replace_all(line, r.device_instance, [](const boost::smatch &m) { return m[1].str() + "\\<instance>"; });
      line = replace_all(line, r.device_path, [](const boost::smatch &m) { return m[1].str() + "<device-path>"; });
      line = replace_all(line, r.guid, [](const boost::smatch &) { return std::string("<id>"); });
      line = replace_all(line, r.sid, [](const boost::smatch &) { return std::string("<sid>"); });
      line = replace_all(line, r.steam, [](const boost::smatch &) { return std::string("<id>"); });
      line = kv_sub(line, r.re_ssid, never, [](const std::string &k, const std::string &sep) { return k + sep + "\"<ssid>\""; });
      line = kv_sub(line, r.wifi_quoted, [](const std::string &, const std::string &) { return true; }, [](const std::string &k, const std::string &sep) { return k + sep + "\"<ssid>\""; });
      line = kv_sub(line, r.re_account, never, [](const std::string &k, const std::string &sep) { return k + sep + "<account>"; });
      line = kv_sub(line, r.re_host, never, [](const std::string &k, const std::string &sep) { return k + sep + "<host>"; });
      line = replace_all(line, r.win_user, [](const boost::smatch &m) { return m[1].str() + ":" + m[2].str() + "Users" + m[2].str() + "<user>"; });
      line = replace_all(line, r.posix_user, [](const boost::smatch &m) { return "/" + m[1].str() + "/<user>"; });
      line = replace_all(line, r.unc, [](const boost::smatch &) { return std::string("\\\\<host>\\<share>"); });
      line = replace_all(line, r.email, [](const boost::smatch &) { return std::string("<email>"); });
      line = replace_all(line, r.local_host, [](const boost::smatch &) { return std::string("<host>"); });
      line = replace_all(line, r.mac, [&](const boost::smatch &m) {
        auto v = m[0].str();
        std::replace(v.begin(), v.end(), '-', ':');
        return labels.of("mac", v);
      });
      {
        const std::string src = line;
        line = replace_all(src, r.ip_any, [&](const boost::smatch &m) {
          const auto v = m[0].str();
          if (v.find(':') != std::string::npos) {
            if (v == "::" || v == "::1") return v;
          if (v.find_first_of("0123456789") == std::string::npos) {
            // no decimal digit: C++ scopes such as a::b look the same; an address is made of groups of 3 or 4 hex characters (fdab::cafe)
            std::size_t start = 0;
            bool all_long = true;
            while (start <= v.size()) {
              const auto end = v.find(':', start);
              const auto len = (end == std::string::npos ? v.size() : end) - start;
              if (len > 0 && len < 3) all_long = false;
              if (end == std::string::npos) break;
              start = end + 1;
            }
            if (!all_long) return v;
          }
            if (boost::regex_match(v, r.clock)) return v;
            return labels.of("ip", v);
          }
          if (KEEP_IPV4.count(v)) return v;
          const auto at = static_cast<std::size_t>(m[0].first - src.cbegin());
          if (boost::regex_search(src.substr(at >= 14 ? at - 14 : 0, at >= 14 ? 14 : at), r.versionish)) return v;
          return labels.of("ip", v);
        });
      }
      {
        // long opaque values: keys, hashes of secrets, session ids; 40 hex characters after commit/sha/hash/build stay (build identity)
        const std::string src = line;
        line = replace_all(src, r.opaque, [&](const boost::smatch &m) {
          const auto v = m[0].str();
          const auto at = static_cast<std::size_t>(m[0].first - src.cbegin());
          if (boost::regex_match(v, r.hex40) && boost::regex_search(src.substr(at >= 20 ? at - 20 : 0, at >= 20 ? 20 : at), r.commit_ctx)) return v;
          if (boost::regex_match(v, r.letters)) return v;
          return std::string("<id>");
        });
      }
      return line;
    }
  }  // namespace

  std::string
  sanitize_utf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t p = 0; p < raw.size();) {
      std::uint32_t cp = 0;
      const auto n = utf8_at(raw, p, cp);
      if (n == 0) {
        out += "\xEF\xBF\xBD";  // U+FFFD
        ++p;
        continue;
      }
      if (!is_dropped_control(cp)) out.append(raw.substr(p, n));
      p += n;
    }
    return out;
  }

  result_t
  scrub(std::string_view raw, std::size_t max_chars, std::size_t max_line) {
    result_t result;
    std::string text = sanitize_utf8(raw);
    // CRLF and lone CR become LF
    {
      std::string t;
      t.reserve(text.size());
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
          t += '\n';
          if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        }
        else t += text[i];
      }
      text = std::move(t);
    }
    bool in_pem_at_start = false;
    if (text.size() > max_chars) {
      // the cut may fall inside a key block: its body must not turn into ordinary lines
      {
        const auto cut = text.size() - max_chars;
        const std::string before = text.substr(0, cut);
        std::size_t begin_at = std::string::npos, end_at = std::string::npos;
        for (boost::sregex_iterator it(before.cbegin(), before.cend(), rules().pem_begin), end; it != end; ++it) begin_at = (std::size_t) it->position(std::size_t { 0 });
        for (boost::sregex_iterator it(before.cbegin(), before.cend(), rules().pem_end), end; it != end; ++it) end_at = (std::size_t) it->position(std::size_t { 0 });
        in_pem_at_start = begin_at != std::string::npos && (end_at == std::string::npos || end_at < begin_at);
      }
      text = text.substr(text.size() - max_chars);
      // from the next line start, and never inside a UTF-8 sequence
      const auto nl = text.find('\n');
      text = nl == std::string::npos ? std::string() : text.substr(nl + 1);
      result.truncated = true;
    }
    labels_t labels;
    std::vector<std::string> users, hosts;
    learn_names(text, users, hosts);
    // The learned names go line by line, after the key blocks are told apart: a name must not change the markers of a key block (a user called "Key" and
    // -----BEGIN PRIVATE KEY-----), and the lines of a key block are dropped, not renamed.
    std::vector<std::pair<regex, std::string>> name_rules;
    for (int pass = 0; pass < 2; ++pass) {
      auto &names = pass == 0 ? users : hosts;
      if (names.empty()) continue;
      std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return a.size() > b.size(); });
      std::string alt;
      for (const auto &n : names) alt += (alt.empty() ? "" : "|") + escape_re(n);
      name_rules.emplace_back(regex("(?<![\\w])(?:" + alt + ")(?![\\w])", I), pass == 0 ? "user" : "host");
    }
    // an occurrence followed by = or : stays only when the name is itself a key word (username=password, then password=...): the key rules need it.
    // Any other name before a colon is part of a message ("Tanaka: disconnected") and goes.
    auto rename = [&](const std::string &line) {
      std::string out_line = line;
      for (const auto &[re, kind] : name_rules) {
        out_line = replace_all(out_line, re, [&](const boost::smatch &m) {
          const auto name = m[0].str();
          if (boost::regex_search(m.suffix().first, m.suffix().second, rules().sep_follows, boost::match_continuous) && boost::regex_match(name, rules().name_is_key)) return name;
          return labels.of(kind, name);
        });
      }
      return out_line;
    };
    std::string out;
    out.reserve(text.size());
    bool in_pem = in_pem_at_start;
    std::size_t lines = 0;
    for (std::size_t pos = 0; pos <= text.size();) {
      auto end = text.find('\n', pos);
      const bool last = end == std::string::npos;
      if (last) end = text.size();
      std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      std::string piece;
      if (in_pem) {
        if (boost::regex_search(line, rules().pem_end)) in_pem = false;
        if (last) break;
        continue;
      }
      if (boost::regex_search(line, rules().pem_begin)) {
        if (!boost::regex_search(line, rules().pem_end)) in_pem = true;
        const auto b = line.find("-----BEGIN");
        piece = scrub_line(rename(line.substr(0, b)), labels) + "<pem>";  // what stands in front of the block is a log line like any other
      }
      else {
        piece = scrub_line(rename(line), labels);
        if (piece.size() > max_line) {
          // never inside a UTF-8 sequence
          std::size_t cut = max_line - 1;
          while (cut > 0 && (static_cast<unsigned char>(piece[cut]) & 0xC0) == 0x80) --cut;
          piece = piece.substr(0, cut) + "\xE2\x80\xA6";
        }
      }
      if (lines++) out += '\n';
      out += piece;
      if (last) break;
    }
    result.text = std::move(out);
    result.lines = lines;
    return result;
  }
}  // namespace crash::logscrub
