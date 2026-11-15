/**
 * @file src/telemetry/error_report.cpp
 * @brief See error_report.h.
 */
#include "error_report.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>

#ifdef _WIN32
  #include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
  #include <dlfcn.h>
  #include <execinfo.h>
#endif

namespace telemetry {

  using nlohmann::json;

  std::string_view
  crumb_name(crumb_e kind) {
    switch (kind) {
      case crumb_e::nav:
        return "nav";
      case crumb_e::act:
        return "act";
      case crumb_e::set:
        return "set";
      case crumb_e::stream:
        return "stream";
      case crumb_e::dev:
        return "dev";
      case crumb_e::state:
        return "state";
      case crumb_e::net:
        return "net";
    }
    return "state";
  }

  namespace {
    // an error code keeps its case: letters, digits and _ . : + -
    std::string
    code_word(std::string_view text, std::size_t max) {
      std::string out;
      for (const unsigned char c : text) {
        if (out.size() >= max) break;
        if ((std::isalnum(c) && c < 0x80) || c == '_' || c == '.' || c == ':' || c == '+' || c == '-') out.push_back(static_cast<char>(c));
      }
      return out;
    }
  }  // namespace

  std::string
  crumb_word(std::string_view text, std::size_t max) {
    std::string out;
    for (const unsigned char c : text) {
      if (out.size() >= max) break;
      if (std::isalnum(c) && c < 0x80)
        out.push_back(static_cast<char>(std::tolower(c)));
      else if (c == '_' || c == '.' || c == ':' || c == '+' || c == '-')
        out.push_back(static_cast<char>(c));
      else if (!out.empty() && out.back() != '_')
        out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
  }

  breadcrumbs_t::breadcrumbs_t(std::size_t capacity):
      capacity_(std::max<std::size_t>(1, capacity)) {}

  void
  breadcrumbs_t::add(crumb_e kind, std::string_view id, std::int64_t now_ms) {
    auto word = crumb_word(id);
    if (word.empty()) return;
    std::lock_guard lock { mutex_ };
    items_.push_back({ now_ms, kind, std::move(word) });
    while (items_.size() > capacity_) items_.pop_front();
  }

  std::vector<std::string>
  breadcrumbs_t::snapshot(std::int64_t now_ms) const {
    std::lock_guard lock { mutex_ };
    std::vector<std::string> out;
    out.reserve(items_.size());
    for (const auto &i : items_) out.push_back(std::to_string(std::clamp<std::int64_t>((now_ms - i.ms) / 1000, 0, 99999)) + "|" + std::string { crumb_name(i.kind) } + "|" + i.id);
    return out;
  }

  void
  breadcrumbs_t::clear() {
    std::lock_guard lock { mutex_ };
    items_.clear();
  }

  std::string
  function_word(std::string_view signature) {
    auto s = std::string { signature };
    if (const auto paren = s.find('('); paren != std::string::npos) s.resize(paren);
    if (const auto space = s.find_last_of(" *&"); space != std::string::npos) s.erase(0, space + 1);
    std::string out;
    for (const unsigned char c : s)
      if (std::isalnum(c) || c == '_' || c == ':' || c == '~' || c == '<' || c == '>' || c == '$' || c == '.') out.push_back(static_cast<char>(c));
    return out.size() > 60 ? out.substr(0, 60) : out;
  }

  std::string
  location_of(const std::source_location &where) {
    std::string file = where.file_name() ? where.file_name() : "";
    std::replace(file.begin(), file.end(), '\\', '/');
    std::size_t at = std::string::npos;
    for (const char *root : { "/src_assets/", "/src/", "/tools/", "/tests/", "/third-party/" }) {
      const auto found = file.rfind(root);
      if (found != std::string::npos && (at == std::string::npos || found < at)) at = found;
    }
    if (at == std::string::npos) {
      // a relative path that already starts at a root (the build passed it that way)
      bool rooted = false;
      for (const char *root : { "src_assets/", "src/", "tools/", "tests/", "third-party/" })
        if (file.rfind(root, 0) == 0) rooted = true;
      if (!rooted) return {};
    }
    else {
      file.erase(0, at + 1);
    }
    return clean_frame(file + ":" + std::to_string(where.line()) + ":" + function_word(where.function_name()));
  }

  std::string
  clean_frame(std::string_view frame) {
    if (frame.empty() || frame.size() > 120) return {};
    // Only four shapes leave, each of fixed parts; anything else (an absolute path, a drive letter, a home folder, a `..` segment) is dropped whole, never repaired:
    //   module+0xoffset                 a native frame: the file name of the module and the offset
    //   src/dir/file.cpp:123[:function] a location in this source tree (the first segment is one of our roots)
    //   function@file.js:12:34          a page frame: the file name only, no folder
    //   file.js:12:34                   the location of a page frame
    static const std::regex native(R"(^[A-Za-z0-9_.-]{1,64}\+0x[0-9a-fA-F]{1,16}$)");
    static const std::regex source(R"(^(?:src|src_assets|src-tauri|tools|tests|third-party)(?:/[A-Za-z0-9_.+-]{1,64}){1,8}:[0-9]{1,7}(?::[A-Za-z0-9_:<>~$.]{0,80})?$)");
    static const std::regex page(R"(^(?:[A-Za-z0-9_$.<>-]{1,40}@)?[A-Za-z0-9_.-]{1,64}:[0-9]{1,7}:[0-9]{1,7}$)");
    const std::string text { frame };
    if (text.find("..") != std::string::npos) return {};  // no path traversal, and no name of ours has two dots in a row
    if (std::regex_match(text, native) || std::regex_match(text, source) || std::regex_match(text, page)) return text;
    return {};
  }

  std::string
  fingerprint_of(std::string_view component, std::string_view code, const std::vector<std::string> &frames) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](std::string_view text) {
      for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
      }
      h ^= 0xFF;
      h *= 1099511628211ULL;
    };
    mix(component);
    mix(code);
    int used = 0;
    for (const auto &f : frames) {
      if (used >= 3) break;
      // a native frame is module+offset: the module only (the offset moves with every build); a location keeps file and function, not the line
      auto key = f;
      if (const auto plus = key.find("+0x"); plus != std::string::npos)
        key.resize(plus);
      else {
        const auto first = key.find(':');
        const auto second = first == std::string::npos ? std::string::npos : key.find(':', first + 1);
        key = key.substr(0, first) + (second == std::string::npos ? "" : key.substr(second));
      }
      mix(key);
      ++used;
    }
    char out[17];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return out;
  }

  json
  error_reporter_t::props_of(const group_t &g, const std::string &fingerprint, std::uint64_t count, bool detail) const {
    json p {
      { "component", crumb_word(g.error.component, 32) },
      { "lang", g.error.lang },
      { "fingerprint", fingerprint },
      { "code", g.error.code },
      { "severity", g.error.severity },
      { "count", std::max<std::uint64_t>(1, count) },
    };
    if (!g.error.loc.empty()) p["loc"] = g.error.loc;
    if (!g.error.build.empty()) p["build"] = crumb_word(g.error.build, 40);
    if (!g.error.page.empty()) p["page"] = crumb_word(g.error.page, 48);
    // the stack and the actions go with the first report of an error only; a count of later ones carries the group and the number
    if (detail && !g.error.frames.empty()) p["frames"] = std::vector<std::string>(g.error.frames.begin(), g.error.frames.begin() + std::min(g.error.frames.size(), MAX_FRAMES));
    if (detail && !g.crumbs.empty()) p["crumbs"] = std::vector<std::string>(g.crumbs.begin() + (g.crumbs.size() > MAX_BREADCRUMBS ? g.crumbs.size() - MAX_BREADCRUMBS : 0), g.crumbs.end());
    return p;
  }

  std::optional<json>
  error_reporter_t::report(const error_input_t &error, const std::vector<std::string> &crumbs, std::int64_t now_ms) {
    std::lock_guard lock { mutex_ };
    auto input = error;
    input.code = code_word(input.code, 64);
    if (input.code.empty()) return std::nullopt;
    const auto fp = fingerprint_of(input.component, input.code, input.frames.empty() && !input.loc.empty() ? std::vector<std::string> { input.loc } : input.frames);
    auto &g = groups_[fp];
    const bool first = g.last_sent_ms == 0;
    g.error = input;
    g.crumbs = crumbs;
    while (!sent_.empty() && now_ms - sent_.front() >= 3'600'000) sent_.pop_front();
    if (first && static_cast<int>(sent_.size()) < MAX_PER_HOUR) {
      g.last_sent_ms = now_ms;
      sent_.push_back(now_ms);
      return props_of(g, fp, 1, true);
    }
    ++g.held;
    if (groups_.size() > 200) groups_.erase(groups_.begin());
    return std::nullopt;
  }

  void
  error_reporter_t::clear() {
    std::lock_guard lock { mutex_ };
    groups_.clear();
    sent_.clear();
  }

  std::vector<json>
  error_reporter_t::flush(std::int64_t now_ms) {
    std::lock_guard lock { mutex_ };
    std::vector<json> out;
    for (auto &[fp, g] : groups_) {
      if (g.held == 0) continue;
      if (g.last_sent_ms != 0 && now_ms - g.last_sent_ms < AGGREGATE_MS) continue;
      while (!sent_.empty() && now_ms - sent_.front() >= 3'600'000) sent_.pop_front();
      if (static_cast<int>(sent_.size()) >= MAX_PER_HOUR) break;
      out.push_back(props_of(g, fp, g.held, false));
      g.held = 0;
      g.last_sent_ms = now_ms;
      sent_.push_back(now_ms);
    }
    return out;
  }

  std::vector<std::string>
  capture_frames(std::size_t skip) {
    std::vector<std::string> out;
#ifdef _WIN32
    void *addresses[MAX_FRAMES + 8] {};
    const auto n = RtlCaptureStackBackTrace(static_cast<DWORD>(skip), static_cast<DWORD>(MAX_FRAMES), addresses, nullptr);
    for (USHORT i = 0; i < n; ++i) {
      HMODULE module = nullptr;
      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(addresses[i]), &module) || !module) continue;
      char path[MAX_PATH] {};
      if (!GetModuleFileNameA(module, path, MAX_PATH)) continue;
      std::string name = path;
      if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name.erase(0, slash + 1);
      char tail[24];
      std::snprintf(tail, sizeof tail, "+0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addresses[i]) - reinterpret_cast<std::uintptr_t>(module)));
      if (auto frame = clean_frame(name + tail); !frame.empty()) out.push_back(std::move(frame));
    }
#elif defined(__linux__) || defined(__APPLE__)
    void *addresses[MAX_FRAMES + 8] {};
    const int n = backtrace(addresses, static_cast<int>(MAX_FRAMES + skip));
    for (int i = static_cast<int>(skip); i < n; ++i) {
      Dl_info info {};
      if (!dladdr(addresses[i], &info) || !info.dli_fname || !info.dli_fbase) continue;
      std::string name = info.dli_fname;
      if (const auto slash = name.find_last_of('/'); slash != std::string::npos) name.erase(0, slash + 1);
      char tail[24];
      std::snprintf(tail, sizeof tail, "+0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addresses[i]) - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
      if (auto frame = clean_frame(name + tail); !frame.empty()) out.push_back(std::move(frame));
    }
#else
    (void) skip;
#endif
    if (out.size() > MAX_FRAMES) out.resize(MAX_FRAMES);
    return out;
  }

  std::optional<error_input_t>
  installer_failure_report(std::string_view result_json) {
    const auto doc = nlohmann::json::parse(result_json, nullptr, false);
    if (!doc.is_object()) return std::nullopt;
    const auto text = [&](const char *key) -> std::string { return doc.contains(key) && doc[key].is_string() ? doc[key].get<std::string>() : std::string {}; };
    const auto state = text("state");
    if (state != "failed" && state != "service-failed" && state != "component-failed" && state != "unknown") return std::nullopt;
    const auto step = text("failed_step");
    const auto code = text("error_code");
    if (step != "payload" && step != "engine" && step != "webview2") return std::nullopt;
    static const std::regex shape(R"(^(?:payload_invalid|(?:engine|webview2)_exit_\d{1,10})$)");
    if (!std::regex_match(code, shape)) return std::nullopt;
    if (code.rfind(step, 0) != 0) return std::nullopt;  // the code word belongs to its step
    error_input_t e;
    e.component = "installer";
    e.lang = "installer";
    e.code = code;
    e.severity = "error";
    e.frames.push_back("installer/" + step);
    return e;
  }

  std::vector<error_input_t>
  take_installer_failures(const std::filesystem::path &root, std::size_t limit) {
    namespace fs = std::filesystem;
    std::vector<error_input_t> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto &run : fs::directory_iterator(root, ec)) {
      if (out.size() >= limit) break;
      const auto file = run.path() / "install-result.json";
      if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > 4096) continue;
      std::ifstream in(file, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const auto report = installer_failure_report(text);
      std::error_code rename_error;
      fs::rename(file, run.path() / "install-result.reported.json", rename_error);  // taken, whatever it said: a success or a cancel is not looked at again either
      if (!rename_error && report) out.push_back(*report);
    }
    return out;
  }

}  // namespace telemetry
