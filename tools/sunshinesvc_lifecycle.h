/**
 * @file tools/sunshinesvc_lifecycle.h
 * @brief The lifecycle of the core as the service sees it, written to a small local file that the host reports at its next start.
 *
 * <install>\config\telemetry\service_lifecycle.jsonl, one JSON line per fact (src/telemetry/service_lifecycle.h reads it):
 *   {"v":1,"kind":"svc_start","t":<unix ms>}
 *   {"v":1,"kind":"core_start","t":..,"how":"launched|adopted","pid":N,"age_ms":N,"cause":"boot|core_exit|session_change|start_failed"}
 *   {"v":1,"kind":"extra_ended","t":..,"n":N}
 *   {"v":1,"kind":"core_exit","t":..,"exit":N,"life_ms":N,"since_adopt_ms":N,"requested":true|false,"restart":true|false}
 * `requested`: the service asked the core to end (service stop, change of the console session). `restart`: the core asked for its own restart (Panel, a saved setting) and exited with code 0 so the
 * service starts it again; the core leaves a marker file (restart_requested, the time in ms) just before it exits and the service takes it when it sees the exit.
 * Only numbers and fixed words are written. Portable code (std only), so it is unit tested on Linux; the service passes the values it reads from the process handles.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace sunshinesvc::lifecycle {

  /// The file never grows past this: when it would, the oldest half is dropped (a service that restarts the core in a loop must not fill the disk).
  inline constexpr std::size_t MAX_FILE_BYTES = 32 * 1024;

  inline std::int64_t
  unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  inline std::string
  num(long long v) {
    return std::to_string(v);
  }

  inline std::string
  svc_start_line(std::int64_t t) {
    return "{\"v\":1,\"kind\":\"svc_start\",\"t\":" + num(t) + "}";
  }

  /// how: "launched" | "adopted"; cause: "boot" | "core_exit" | "session_change" | "start_failed". age_ms is the age of the process (for an adopted core, how long it had been running).
  inline std::string
  core_start_line(std::int64_t t, bool adopted, unsigned long pid, long long age_ms, const char *cause) {
    return "{\"v\":1,\"kind\":\"core_start\",\"t\":" + num(t) + ",\"how\":\"" + (adopted ? "adopted" : "launched") + "\",\"pid\":" + num(pid) + ",\"age_ms\":" + num(age_ms) + ",\"cause\":\"" + cause + "\"}";
  }

  inline std::string
  extra_ended_line(std::int64_t t, int n) {
    return "{\"v\":1,\"kind\":\"extra_ended\",\"t\":" + num(t) + ",\"n\":" + num(n) + "}";
  }

  /// exit_code is written as the signed 32-bit value of the DWORD (a negative code is an HRESULT or NTSTATUS); since_adopt_ms is -1 for a core the service started itself.
  inline std::string
  core_exit_line(std::int64_t t, unsigned long exit_code, long long life_ms, long long since_adopt_ms, bool requested, bool restart = false, const std::string &restart_reason = {}) {
    return "{\"v\":1,\"kind\":\"core_exit\",\"t\":" + num(t) + ",\"exit\":" + num(static_cast<std::int32_t>(exit_code)) + ",\"life_ms\":" + num(life_ms) + ",\"since_adopt_ms\":" + num(since_adopt_ms) + ",\"requested\":" + (requested ? "true" : "false") + ",\"restart\":" + (restart ? "true" : "false") + (restart_reason.empty() ? "" : ",\"reason\":\"" + restart_reason + "\"") + "}";
  }

  /// A restart the core asked for itself ends with exit code 0, which looks like an exit nobody asked for. The marker tells the two apart; it is only valid for a minute.
  inline constexpr const char *RESTART_MARKER = "restart_requested";
  inline constexpr std::int64_t RESTART_MARKER_WINDOW_MS = 60'000;

  /// Appends one line; keeps the file under MAX_FILE_BYTES. Never throws: a service that cannot write its notes carries on.
  inline bool
  append(const std::filesystem::path &file, const std::string &line) noexcept {
    try {
      std::error_code ec;
      std::filesystem::create_directories(file.parent_path(), ec);
      const auto size = std::filesystem::exists(file, ec) ? std::filesystem::file_size(file, ec) : 0;
      if (!ec && size + line.size() + 1 > MAX_FILE_BYTES) {
        std::string text;
        {
          std::ifstream in { file, std::ios::binary };
          text.assign(std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {});
        }
        text.erase(0, text.size() / 2);
        const auto nl = text.find('\n');
        if (nl != std::string::npos) text.erase(0, nl + 1);
        std::ofstream out { file, std::ios::binary | std::ios::trunc };
        out << text;
      }
      std::ofstream out { file, std::ios::binary | std::ios::app };
      if (!out) return false;
      out << line << '\n';
      return static_cast<bool>(out);
    }
    catch (...) {
      return false;
    }
  }

  /**
   * Called by the core right before it exits for a restart it was asked to do. The note holds the time, the id and the start time of this process and the reason word, so that only the exit
   * of this very core takes it (a note left by a standalone core that was restarted by hand must not turn the exit of another core into a "requested" restart). Never throws.
   */
  inline bool
  note_restart_request(const std::filesystem::path &dir, std::int64_t t, const char *reason = "api", unsigned long pid = 0, std::int64_t start_ms = 0) noexcept {
    try {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream out { dir / RESTART_MARKER, std::ios::binary | std::ios::trunc };
      if (!out) return false;
      out << num(t) << ' ' << num(static_cast<long long>(pid)) << ' ' << num(start_ms) << ' ' << (reason ? reason : "api") << '\n';
      return static_cast<bool>(out);
    }
    catch (...) {
      return false;
    }
  }

  /**
   * Called by the service when a core ended: true when that core (`pid`, `start_ms`) had left a fresh note (it is removed either way, so a note never outlives the next exit).
   * `exit_t` is the time of the exit.
   */
  inline bool
  take_restart_request(const std::filesystem::path &dir, std::int64_t exit_t, unsigned long pid = 0, std::int64_t start_ms = 0, std::string *reason = nullptr) noexcept {
    try {
      const auto file = dir / RESTART_MARKER;
      std::string text;
      {
        std::ifstream in { file, std::ios::binary };
        if (!in) return false;
        text.assign(std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {});
      }
      std::error_code ec;
      std::filesystem::remove(file, ec);
      std::size_t i = 0;
      const auto number = [&](long long limit) -> long long {
        long long v = 0;
        bool any = false;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
          v = v * 10 + (text[i] - '0');
          any = true;
          if (v > limit) return -1;
        }
        if (i < text.size() && text[i] == ' ') ++i;
        return any ? v : -1;
      };
      const auto t = number(4'102'444'800'000LL);
      const auto note_pid = number(4'294'967'295LL);
      const auto note_start = number(4'102'444'800'000LL);
      if (t <= 0 || note_pid < 0 || note_start < 0) return false;
      if (reason) {
        // one lower-case word at the end; the host checks it against its own list
        std::string word;
        for (; i < text.size() && word.size() < 24 && text[i] >= 'a' && text[i] <= 'z'; ++i) word.push_back(text[i]);
        *reason = word;
      }
      if (static_cast<unsigned long>(note_pid) != pid || note_start != start_ms) return false;  // the note of another core
      return exit_t >= t - 2000 && exit_t - t <= RESTART_MARKER_WINDOW_MS;
    }
    catch (...) {
      return false;
    }
  }

#ifdef _WIN32
  /// When a process started, as unix ms (0 when it cannot be read): with its id it names one process for good, an id alone can be reused.
  inline std::int64_t
  process_start_unix_ms(HANDLE process) {
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER v;
    v.LowPart = created.dwLowDateTime;
    v.HighPart = created.dwHighDateTime;
    return static_cast<std::int64_t>((v.QuadPart - 116444736000000000ULL) / 10000ULL);
  }
#endif

}  // namespace sunshinesvc::lifecycle
