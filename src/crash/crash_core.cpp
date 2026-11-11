/**
 * @file src/crash/crash_core.cpp
 * @brief Crash report meta: frames, signature, JSON, consent and the pending folder.
 */
#include "crash_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace crash {
  namespace fs = std::filesystem;

  namespace {
    constexpr std::uint32_t k256[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    inline std::uint32_t
    rotr(std::uint32_t x, unsigned n) {
      return (x >> n) | (x << (32 - n));
    }

    void
    sha256_block(std::uint32_t h[8], const std::uint8_t *p) {
      std::uint32_t w[64];
      for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t) p[i * 4] << 24 | (std::uint32_t) p[i * 4 + 1] << 16 | (std::uint32_t) p[i * 4 + 2] << 8 | (std::uint32_t) p[i * 4 + 3];
      }
      for (int i = 16; i < 64; ++i) {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; ++i) {
        const auto S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const auto ch = (e & f) ^ (~e & g);
        const auto t1 = hh + S1 + ch + k256[i] + w[i];
        const auto S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const auto mj = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = S0 + mj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
      }
      h[0] += a;
      h[1] += b;
      h[2] += c;
      h[3] += d;
      h[4] += e;
      h[5] += f;
      h[6] += g;
      h[7] += hh;
    }

    /// Fixed-capacity writer: no allocation, never overruns, remembers when something did not fit.
    struct writer_t {
      char *out;
      std::size_t cap;
      std::size_t len = 0;
      bool full = false;

      void
      put(char c) {
        if (len + 1 >= cap) {
          full = true;
          return;
        }
        out[len++] = c;
      }

      void
      raw(const char *s) {
        while (*s) put(*s++);
      }

      void
      str(const char *s) {
        put('"');
        for (; s && *s; ++s) {
          const auto c = (unsigned char) *s;
          if (c == '"' || c == '\\') {
            put('\\');
            put((char) c);
          }
          else if (c < 0x20) {
            raw("\\u00");
            put("0123456789abcdef"[c >> 4]);
            put("0123456789abcdef"[c & 15]);
          }
          else {
            put((char) c);
          }
        }
        put('"');
      }

      void
      dec(std::uint64_t v) {
        char tmp[24];
        int n = 0;
        do {
          tmp[n++] = (char) ('0' + v % 10);
          v /= 10;
        } while (v);
        while (n) put(tmp[--n]);
      }

      /// "0x1f" as a JSON string: JSON has no hex literal and 64-bit offsets do not survive a double
      void
      hex(std::uint64_t v) {
        char tmp[20];
        int n = 0;
        do {
          tmp[n++] = "0123456789abcdef"[v & 15];
          v >>= 4;
        } while (v);
        put('"');
        raw("0x");
        while (n) put(tmp[--n]);
        put('"');
      }

      /// the same digits, unquoted, for the text the signature hashes
      void
      hex_bare(std::uint64_t v) {
        char tmp[20];
        int n = 0;
        do {
          tmp[n++] = "0123456789abcdef"[v & 15];
          v >>= 4;
        } while (v);
        raw("0x");
        while (n) put(tmp[--n]);
      }
    };

    char
    lower(char c) {
      return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
    }

    bool
    iequal(std::string_view a, std::string_view b) {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
      }
      return true;
    }

    std::string_view
    trim(std::string_view s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
      return s;
    }
  }  // namespace

  void
  sha256(const void *data, std::size_t size, std::uint8_t out[32]) {
    std::uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const auto *p = (const std::uint8_t *) data;
    std::size_t left = size;
    while (left >= 64) {
      sha256_block(h, p);
      p += 64;
      left -= 64;
    }
    std::uint8_t tail[128] = {};
    std::memcpy(tail, p, left);
    tail[left] = 0x80;
    const std::size_t tail_len = left < 56 ? 64 : 128;
    const std::uint64_t bits = (std::uint64_t) size * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = (std::uint8_t) (bits >> (8 * i));
    sha256_block(h, tail);
    if (tail_len == 128) sha256_block(h, tail + 64);
    for (int i = 0; i < 8; ++i) {
      out[i * 4] = (std::uint8_t) (h[i] >> 24);
      out[i * 4 + 1] = (std::uint8_t) (h[i] >> 16);
      out[i * 4 + 2] = (std::uint8_t) (h[i] >> 8);
      out[i * 4 + 3] = (std::uint8_t) h[i];
    }
  }

  bool
  is_runtime_module(std::string_view module) {
    static constexpr std::string_view names[] = {
      "ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll", "msvcrt.dll", "vcruntime140.dll", "vcruntime140_1.dll",
      "msvcp140.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "libc.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6",
    };
    for (const auto name : names) {
      if (iequal(module, name)) return true;
    }
    // Linux sonames carry their version: libstdc++.so.6.0.36, libc-2.39.so, ld-linux-x86-64.so.2
    static constexpr std::string_view prefixes[] = { "libstdc++.so", "libgcc_s.so", "libc.so", "libc-", "libm.so", "libm-", "ld-linux", "libpthread" };
    for (const auto prefix : prefixes) {
      if (module.substr(0, prefix.size()) == prefix) return true;
    }
    return false;
  }

  std::size_t
  first_caller_frame(const frame_t *frames, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      if (!is_runtime_module(frames[i].module)) return i;
    }
    return 0;
  }

  namespace {
    /// The part of a module name after the last directory separator: a module is named by its file, never by where it lies (a path holds the account name).
    const char *
    base_name(const char *module) {
      const char *base = module;
      for (const char *c = module; *c; ++c)
        if (*c == 0x2f || *c == 0x5c) base = c + 1;
      return base;
    }
  }  // namespace

  void
  signature_hex(const frame_t *frames, std::size_t count, char out[65]) {
    // 8 frames of at most (64 + 1 + 18 + 1) bytes
    char text[8 * (module_name_size + 24)];
    writer_t w { text, sizeof(text) };
    const auto from = first_caller_frame(frames, count);
    for (std::size_t i = from, n = 0; i < count && n < signature_frames; ++i, ++n) {
      if (n) w.put('\n');
      for (const char *m = base_name(frames[i].module); *m; ++m) w.put(lower(*m));
      w.put('+');
      w.hex_bare(frames[i].offset);
    }
    std::uint8_t digest[32];
    sha256(text, w.len, digest);
    for (int i = 0; i < 32; ++i) {
      out[i * 2] = "0123456789abcdef"[digest[i] >> 4];
      out[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
    }
    out[64] = 0;
  }

  void
  format_iso_utc(std::int64_t t, char out[24]) {
    // civil-from-days (Howard Hinnant), no gmtime: it is neither re-entrant everywhere nor allowed in a crash handler
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) {
      rem += 86400;
      --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = (unsigned) (days - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = (std::int64_t) yoe + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto d = doy - (153 * mp + 2) / 5 + 1;
    const auto m = mp < 10 ? mp + 3 : mp - 9;
    const auto year = (int) (y + (m <= 2));
    const int hh = (int) (rem / 3600), mm = (int) (rem % 3600 / 60), ss = (int) (rem % 60);
    auto two = [](char *p, int v) {
      p[0] = (char) ('0' + v / 10 % 10);
      p[1] = (char) ('0' + v % 10);
    };
    out[0] = (char) ('0' + year / 1000 % 10);
    out[1] = (char) ('0' + year / 100 % 10);
    two(out + 2, year % 100);
    out[4] = '-';
    two(out + 5, (int) m);
    out[7] = '-';
    two(out + 8, (int) d);
    out[10] = 'T';
    two(out + 11, hh);
    out[13] = ':';
    two(out + 14, mm);
    out[16] = ':';
    two(out + 17, ss);
    out[19] = 'Z';
    out[20] = 0;
  }

  std::size_t
  render_meta(const meta_t &meta, char *out, std::size_t capacity) {
    writer_t w { out, capacity };
    char signature[65];
    signature_hex(meta.frames, meta.frame_count, signature);
    char iso[24];
    format_iso_utc(meta.time, iso);
    const auto caller = first_caller_frame(meta.frames, meta.frame_count);
    const char *module = meta.frame_count ? base_name(meta.frames[caller].module) : "";
    const std::uint64_t offset = meta.frame_count ? meta.frames[caller].offset : 0;

    w.raw("{\"v\":1,\"product\":");
    w.str(meta.product);
    if (meta.install_id && *meta.install_id) {
      w.raw(",\"install_id\":");
      w.str(meta.install_id);
    }
    w.raw(",\"version\":");
    w.str(meta.version);
    w.raw(",\"os_version\":");
    w.str(meta.os_version);
    w.raw(",\"time\":");
    w.str(iso);
    w.raw(",\"uptime_s\":");
    w.dec(meta.uptime_s);
    w.raw(",\"exception_code\":");
    w.str(meta.fault_code);
    w.raw(",\"module\":");
    w.str(module);
    w.raw(",\"module_offset\":");
    w.hex(offset);
    w.raw(",\"frames\":[");
    const auto count = std::min(meta.frame_count, max_frames);
    for (std::size_t i = 0; i < count; ++i) {
      if (i) w.put(',');
      w.raw("{\"module\":");
      w.str(base_name(meta.frames[i].module));
      w.raw(",\"offset\":");
      w.hex(meta.frames[i].offset);
      w.put('}');
    }
    w.raw("],\"signature\":\"");
    w.raw(signature);
    w.raw("\",\"kind\":");
    w.str(meta.kind);
    w.put('}');
    if (w.full) return 0;
    out[w.len] = 0;
    return w.len;
  }

  std::string
  conf_value(std::string_view text, std::string_view key) {
    std::string found;
    std::size_t pos = 0;
    while (pos <= text.size()) {
      auto end = text.find('\n', pos);
      if (end == std::string_view::npos) end = text.size();
      auto line = trim(text.substr(pos, end - pos));
      pos = end + 1;
      if (line.empty() || line.front() == '#') continue;
      const auto eq = line.find('=');
      if (eq == std::string_view::npos) continue;
      if (trim(line.substr(0, eq)) != key) continue;
      auto value = trim(line.substr(eq + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      found.assign(value);
    }
    return found;
  }

  bool
  errors_enabled_from_conf(std::string_view text) {
    auto falsy = [](const std::string &v) { return iequal(v, "off") || iequal(v, "false") || iequal(v, "0"); };
    const auto own = conf_value(text, "telemetry_errors");
    if (!own.empty()) return !falsy(own);
    // someone who said no to the old single question said no to everything that leaves the computer
    return conf_value(text, "telemetry_consent") != "off";
  }

  std::string
  report_stem(std::int64_t t, std::string_view product, std::string_view kind, std::string_view signature) {
    std::string stem = std::to_string(t) + "-";
    auto safe = [&](std::string_view s) {
      for (const char c : s) stem += (std::isalnum((unsigned char) c) || c == '-') ? c : '_';
    };
    safe(product);
    stem += '-';
    safe(kind);
    stem += '-';
    safe(signature.substr(0, 8));
    return stem;
  }

  namespace {
    std::string
    utf16le_of(std::string_view s) {
      // UTF-8 -> UTF-16LE bytes (an invalid sequence is passed through as it is: it only has to match the same bytes in a dump)
      std::string out;
      auto put = [&](std::uint32_t u) {
        out.push_back((char) (u & 0xff));
        out.push_back((char) ((u >> 8) & 0xff));
      };
      for (std::size_t i = 0; i < s.size();) {
        const auto c = (unsigned char) s[i];
        std::uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xf0 && i + 3 < s.size()) {
          cp = ((c & 7u) << 18) | (((unsigned char) s[i + 1] & 63u) << 12) | (((unsigned char) s[i + 2] & 63u) << 6) | ((unsigned char) s[i + 3] & 63u);
          len = 4;
        }
        else if (c >= 0xe0 && i + 2 < s.size()) {
          cp = ((c & 15u) << 12) | (((unsigned char) s[i + 1] & 63u) << 6) | ((unsigned char) s[i + 2] & 63u);
          len = 3;
        }
        else if (c >= 0xc0 && i + 1 < s.size()) {
          cp = ((c & 31u) << 6) | ((unsigned char) s[i + 1] & 63u);
          len = 2;
        }
        i += len;
        if (cp >= 0x10000) {
          cp -= 0x10000;
          put(0xd800 + (cp >> 10));
          put(0xdc00 + (cp & 0x3ff));
        }
        else
          put(cp);
      }
      return out;
    }

    inline unsigned char
    fold(unsigned char c) {
      return (c >= 'A' && c <= 'Z') ? (unsigned char) (c + 32) : c;
    }

    bool
    matches_at(const std::string &data, std::size_t at, const std::string &pattern) {
      if (at + pattern.size() > data.size()) return false;
      for (std::size_t k = 0; k < pattern.size(); ++k)
        if (fold((unsigned char) data[at + k]) != fold((unsigned char) pattern[k])) return false;
      return true;
    }

    bool
    path_stop(std::uint32_t unit) {
      return unit < 0x20 || unit == 0x7f || unit == '\\' || unit == '/' || unit == ':' || unit == '*' || unit == '?' || unit == '"' || unit == '<' || unit == '>' || unit == '|';
    }

    bool
    word_unit(const std::string &data, std::size_t at, std::size_t unit) {
      if (at + unit > data.size()) return false;
      if (unit == 2 && data[at + 1] != 0) return false;
      const auto c = (unsigned char) data[at];
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    void
    blank(std::string &data, std::size_t from, std::size_t units, std::size_t unit) {
      for (std::size_t k = 0; k < units; ++k) {
        data[from + k * unit] = 'x';
        if (unit == 2) data[from + k * unit + 1] = 0;
      }
    }

    std::size_t
    blank_after_marker(std::string &data, const std::string &marker, std::size_t unit) {
      std::size_t hits = 0;
      for (std::size_t i = 0; i + marker.size() < data.size(); ++i) {
        if (!matches_at(data, i, marker)) continue;
        const std::size_t start = i + marker.size();
        std::size_t units = 0;
        bool all_x = true;
        for (std::size_t j = start; units < 64 && j + unit <= data.size(); j += unit) {
          const std::uint32_t u = unit == 2 ? (std::uint32_t) ((unsigned char) data[j] | ((unsigned char) data[j + 1] << 8)) : (unsigned char) data[j];
          if (path_stop(u)) break;
          if (u != 'x') all_x = false;
          ++units;
        }
        if (units && !all_x) {
          blank(data, start, units, unit);
          ++hits;
        }
        i = start + units * unit;
      }
      return hits;
    }

    std::size_t
    blank_token(std::string &data, const std::string &pattern, std::size_t unit) {
      std::size_t hits = 0;
      const auto first = fold((unsigned char) pattern[0]);
      for (std::size_t i = 0; i + pattern.size() <= data.size(); ++i) {
        if (fold((unsigned char) data[i]) != first || !matches_at(data, i, pattern)) continue;
        if (i >= unit && word_unit(data, i - unit, unit)) continue;
        if (word_unit(data, i + pattern.size(), unit)) continue;
        blank(data, i, pattern.size() / unit, unit);
        ++hits;
        i += pattern.size() - 1;
      }
      return hits;
    }
  }  // namespace

  std::size_t
  redact_private_names(std::string &data, const std::vector<std::string> &names) {
    std::size_t hits = 0;
    for (const char *marker : { "\\Users\\", "/Users/", "/home/" }) {
      hits += blank_after_marker(data, marker, 1);
      hits += blank_after_marker(data, utf16le_of(marker), 2);
    }
    for (const auto &name : names) {
      if (name.size() < 3) continue;
      hits += blank_token(data, name, 1);
      hits += blank_token(data, utf16le_of(name), 2);
    }
    return hits;
  }

  fs::path
  pending_dir(const fs::path &config_dir) {
    return config_dir / "crashes" / "pending";
  }

  std::vector<pending_report_t>
  list_pending(const fs::path &dir) {
    std::vector<pending_report_t> reports;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return reports;
    std::vector<fs::path> dumps;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      std::error_code e2;
      if (!entry.is_regular_file(e2)) continue;
      const auto ext = entry.path().extension().string();
      if (ext == ".dmp") {
        dumps.push_back(entry.path());
        continue;
      }
      if (ext != ".json") continue;
      pending_report_t report;
      report.meta = entry.path();
      report.time = entry.last_write_time(e2);
      report.bytes = entry.file_size(e2);
      auto dump = entry.path();
      dump.replace_extension(".dmp");
      if (fs::is_regular_file(dump, e2)) {
        report.dump = dump;
        report.bytes += fs::file_size(dump, e2);
      }
      reports.push_back(std::move(report));
    }
    for (const auto &dump : dumps) {
      auto meta = dump;
      meta.replace_extension(".json");
      std::error_code e2;
      if (!fs::exists(meta, e2)) fs::remove(dump, e2);
    }
    std::sort(reports.begin(), reports.end(), [](const auto &a, const auto &b) {
      return a.time != b.time ? a.time < b.time : a.meta < b.meta;
    });
    return reports;
  }

  std::size_t
  trim_pending(const fs::path &dir, std::size_t keep, int max_age_days) {
    auto reports = list_pending(dir);
    std::size_t removed = 0;
    const auto now = fs::file_time_type::clock::now();
    auto drop = [&](const pending_report_t &r) {
      std::error_code ec;
      fs::remove(r.meta, ec);
      if (!r.dump.empty()) fs::remove(r.dump, ec);
      ++removed;
    };
    std::vector<pending_report_t> alive;
    for (auto &r : reports) {
      if (max_age_days > 0 && now - r.time > std::chrono::hours(24 * max_age_days)) drop(r);
      else alive.push_back(std::move(r));
    }
    while (alive.size() > keep) {
      drop(alive.front());
      alive.erase(alive.begin());
    }
    return removed;
  }

  void
  clear_pending(const fs::path &dir) {
    trim_pending(dir, 0, 0);
  }

  std::string
  new_uuid_v4() {
    std::random_device rd;
    std::uint8_t b[16];
    for (auto &x : b) x = (std::uint8_t) (rd() & 0xff);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    std::string s;
    for (int i = 0; i < 16; ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
      s += "0123456789abcdef"[b[i] >> 4];
      s += "0123456789abcdef"[b[i] & 15];
    }
    return s;
  }

  std::string
  read_or_create_install_id(const fs::path &config_dir, bool create) {
    const auto path = config_dir / "telemetry" / "install_id";
    auto valid = [](const std::string &s) {
      if (s.size() != 36) return false;
      for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !std::isxdigit((unsigned char) c)) return false;
      }
      return true;
    };
    auto read = [&]() {
      std::ifstream in(path);
      std::string id;
      std::getline(in, id);
      while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
      return valid(id) ? id : std::string {};
    };
    if (auto id = read(); !id.empty()) return id;
    if (!create) return {};
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // write to a private name and hard-link it into place: of two processes only one wins, and both then read the winner
    const auto temp = path.parent_path() / ("install_id." + new_uuid_v4().substr(0, 8) + ".tmp");
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out << new_uuid_v4() << '\n';
    }
    fs::create_hard_link(temp, path, ec);
    std::error_code ignore;
    if (ec && !fs::exists(path, ignore)) fs::rename(temp, path, ignore);  // a file system without hard links
    fs::remove(temp, ignore);
    return read();
  }
}  // namespace crash
