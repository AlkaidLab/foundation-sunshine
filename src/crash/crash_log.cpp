/**
 * @file src/crash/crash_log.cpp
 * @brief The session log that goes with a report (see crash_log.h).
 */
#include "crash_log.h"

#include "log_scrub.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <zlib.h>

namespace crash::logfile {
  namespace fs = std::filesystem;

  namespace {
    std::string
    read_all(const fs::path &path, std::size_t max_bytes = 64u * 1024 * 1024) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      in.seekg(0, std::ios::end);
      const auto size = static_cast<std::size_t>(std::max<std::streamoff>(0, in.tellg()));
      std::size_t skip = size > max_bytes ? size - max_bytes : 0;  // a huge file: the end of it
      in.seekg((std::streamoff) skip);
      std::string out(size - skip, '\0');
      in.read(out.data(), (std::streamsize) out.size());
      out.resize((std::size_t) in.gcount());
      if (skip) {
        const auto nl = out.find('\n');
        out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
      }
      return out;
    }

    /// Index of the first byte of a character at or after `at`.
    std::size_t
    char_start(const std::string &s, std::size_t at) {
      while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) ++at;
      return at;
    }

    std::string
    last_lines(const std::string &text, std::size_t n) {
      std::size_t pos = text.size();
      if (pos && text[pos - 1] == '\n') --pos;
      for (std::size_t seen = 0; seen < n; ++seen) {
        const auto nl = text.rfind('\n', pos ? pos - 1 : 0);
        if (nl == std::string::npos || pos == 0) return text;
        pos = nl;
      }
      return text.substr(pos + 1);
    }
  }  // namespace

  std::string
  gzip(std::string_view data) {
    z_stream zs {};
    if (deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    std::string out;
    out.resize(deflateBound(&zs, (uLong) data.size()) + 32);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    zs.avail_in = (uInt) data.size();
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = (uInt) out.size();
    const int rc = deflate(&zs, Z_FINISH);
    const auto produced = out.size() - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(produced);
    return out;
  }

  std::optional<packed_t>
  pack(std::string_view raw, std::size_t max_gz) {
    auto scrubbed = logscrub::scrub(raw);
    if (scrubbed.text.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
    packed_t p;
    p.truncated = scrubbed.truncated;
    std::string text = std::move(scrubbed.text);
    p.gz = gzip(text);
    // over the limit: drop the oldest half of the lines until it fits
    for (int i = 0; i < 12 && p.gz.size() > max_gz && text.size() > 1024; ++i) {
      const auto nl = text.find('\n', text.size() / 2);
      text = nl == std::string::npos ? text.substr(text.size() / 2) : text.substr(nl + 1);
      text.erase(0, char_start(text, 0));
      p.truncated = true;
      p.gz = gzip(text);
    }
    if (p.gz.empty() || p.gz.size() > max_gz) return std::nullopt;
    p.lines = (std::size_t) std::count(text.begin(), text.end(), '\n') + (text.empty() || text.back() == '\n' ? 0 : 1);
    // head: the first edge_bytes, whole characters; tail: the last edge_bytes, from the start of a line
    std::size_t head_len = std::min(text.size(), edge_bytes);
    while (head_len > 0 && head_len < text.size() && (static_cast<unsigned char>(text[head_len]) & 0xC0) == 0x80) --head_len;
    p.head = text.substr(0, head_len);
    if (text.size() > edge_bytes) {
      std::size_t from = text.size() - edge_bytes;
      const auto nl = text.find('\n', from);
      from = nl == std::string::npos ? char_start(text, from) : nl + 1;
      p.tail = text.substr(from);
    }
    else p.tail = text;
    return p;
  }

  std::vector<fs::path>
  candidates(const fs::path &log_file) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(log_file, ec)) out.push_back(log_file);
    for (int i = 1; i <= 5; ++i) {
      auto p = log_file;
      p += "." + std::to_string(i);
      if (fs::is_regular_file(p, ec)) out.push_back(p);
    }
    return out;
  }

  fs::path
  pick_by_time(const std::vector<fs::path> &files, fs::file_time_type when, std::chrono::seconds window) {
    // The log of the run that crashed was last written when it crashed or before; a file written clearly after the report belongs to the run that came next. A few seconds of
    // slack cover the order in which the handler wrote the report and the log was flushed.
    constexpr auto slack = std::chrono::seconds(30);
    fs::path best;
    std::chrono::seconds best_delta = window + std::chrono::seconds(1);
    for (const auto &f : files) {
      std::error_code ec;
      const auto t = fs::last_write_time(f, ec);
      if (ec) continue;
      const auto signed_delta = std::chrono::duration_cast<std::chrono::seconds>(t - when);
      if (signed_delta > slack) continue;
      const auto delta = std::chrono::abs(signed_delta);
      if (delta <= window && delta < best_delta) {
        best = f;
        best_delta = delta;
      }
    }
    return best;
  }

  std::string
  collect(const fs::path &log_file, fs::file_time_type when) {
    const auto files = candidates(log_file);
    const auto picked = pick_by_time(files, when);
    if (picked.empty()) return {};
    std::string text = read_all(picked);
    // a crash at start-up leaves a log of a few lines: the end of the run before it says what was going on
    if (std::count(text.begin(), text.end(), '\n') < 100) {
      const auto at = std::find(files.begin(), files.end(), picked);
      if (at != files.end() && at + 1 != files.end()) {
        const auto before = last_lines(read_all(*(at + 1)), 200);
        if (!before.empty()) text = "---- end of the run before ----\n" + before + (before.back() == '\n' ? "" : "\n") + "---- this run ----\n" + text;
      }
    }
    return text;
  }

  std::string
  collect_current(const fs::path &log_file) {
    std::error_code ec;
    return fs::is_regular_file(log_file, ec) ? read_all(log_file) : std::string();
  }
}  // namespace crash::logfile
