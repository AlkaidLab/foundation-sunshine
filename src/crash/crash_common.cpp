/**
 * @file src/crash/crash_common.cpp
 * @brief Crash capture: options, consent, the hang watchdog and the test switch (platform-independent part).
 */
#include "crash_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace crash {
  namespace fs = std::filesystem;

  namespace detail {
    state_t g;

    std::uint64_t
    now_ms() {
      return (std::uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::int64_t
    now_unix() {
      return (std::int64_t) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::size_t
    stem_noalloc(char *out, std::size_t cap, std::int64_t t, const char *kind, std::uint32_t pid) {
      std::size_t n = 0;
      auto put = [&](char c) {
        if (n + 1 < cap) out[n++] = c;
      };
      auto dec = [&](std::uint64_t v) {
        char tmp[24];
        int k = 0;
        do {
          tmp[k++] = (char) ('0' + v % 10);
          v /= 10;
        } while (v);
        while (k) put(tmp[--k]);
      };
      auto text = [&](const char *s) {
        for (; *s; ++s) put((std::isalnum((unsigned char) *s) || *s == '-') ? *s : '_');
      };
      dec((std::uint64_t) t);
      put('-');
      text(g.product);
      put('-');
      text(kind);
      put('-');
      put('p');
      for (int shift = 28; shift >= 0; shift -= 4) put("0123456789abcdef"[(pid >> shift) & 15]);
      out[n] = 0;
      return n;
    }
  }  // namespace detail

  using namespace detail;

  namespace {
    std::mutex config_mutex;
    fs::path g_config_dir;
    fs::path g_config_file;
    std::string g_conf_text;
    std::thread g_watchdog;
    std::atomic<bool> g_stop { false };
    std::mutex g_wait_mutex;
    std::condition_variable g_wait_cv;

    void
    copy_to(char *dst, std::size_t cap, const std::string &src) {
      const auto n = std::min(cap - 1, src.size());
      std::memcpy(dst, src.data(), n);
      dst[n] = 0;
    }

    std::string
    read_text(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    void
    watchdog_loop() {
      int ticks = 0;
      while (!g_stop.load()) {
        {
          std::unique_lock lock(g_wait_mutex);
          g_wait_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop.load(); });
        }
        if (g_stop.load()) break;
        ++ticks;
        platform_tick();
        if (ticks % 30 == 0) refresh_config();
        const auto now = now_ms();
        for (int i = 0; i < max_slots; ++i) {
          auto &slot = g.slots[i];
          if (!slot.used.load(std::memory_order_acquire)) continue;
          const auto since = slot.busy_since_ms.load(std::memory_order_acquire);
          if (since == 0) {
            slot.reported.store(false, std::memory_order_relaxed);
            continue;
          }
          if (now - since < slot.timeout_ms || slot.reported.load(std::memory_order_relaxed)) continue;
          slot.reported.store(true, std::memory_order_relaxed);
          if (g.enabled.load()) platform_report_hang(i);
        }
      }
    }
  }  // namespace

  void
  install(const options_t &options) {
    if (g.installed.exchange(true)) return;
    copy_to(g.product, sizeof(g.product), options.product);
    copy_to(g.version, sizeof(g.version), options.version);
    copy_to(g.os_version, sizeof(g.os_version), options.os_version);
    copy_to(g.pending_utf8, sizeof(g.pending_utf8), pending_dir(options.config_dir).string());
    g.start_unix = now_unix();
    g.start_ms = now_ms();
    {
      std::lock_guard lock(config_mutex);
      g_config_dir = options.config_dir;
      g_config_file = options.config_file.empty() ? options.config_dir / "sunshine.conf" : options.config_file;
    }
    std::error_code ec;
    fs::create_directories(pending_dir(options.config_dir), ec);
    refresh_config();
    platform_install();
    g_watchdog = std::thread(watchdog_loop);
  }

  void
  refresh_config() {
    std::string text;
    {
      std::lock_guard lock(config_mutex);
      text = read_text(g_config_file);
      g_conf_text = text;
    }
    const bool on = errors_enabled_from_conf(text);
    const bool was = g.enabled.exchange(on);
    if (was && !on) {
      // turned off: what is on disk goes, a person who said no keeps nothing of this
      fs::path dir;
      {
        std::lock_guard lock(config_mutex);
        dir = pending_dir(g_config_dir);
      }
      clear_pending(dir);
    }
  }

  void
  set_config_file(const fs::path &config_file) {
    {
      std::lock_guard lock(config_mutex);
      g_config_file = config_file;
    }
    refresh_config();
  }

  bool
  enabled() {
    return g.enabled.load();
  }

  int
  watch_register(const char *name, std::uint32_t timeout_ms) {
    for (int i = 0; i < max_slots; ++i) {
      bool expected = false;
      auto &slot = g.slots[i];
      if (!slot.used.compare_exchange_strong(expected, true)) continue;
      slot.busy_since_ms.store(0);
      slot.timeout_ms = timeout_ms;
      slot.reported.store(false);
      std::strncpy(slot.name, name ? name : "thread", sizeof(slot.name) - 1);
      slot.name[sizeof(slot.name) - 1] = 0;
      platform_bind_thread(slot);
      return i;
    }
    return -1;
  }

  void
  watch_release(int slot) {
    if (slot < 0 || slot >= max_slots) return;
    auto &s = g.slots[slot];
    s.busy_since_ms.store(0);
    platform_release_thread(s);
    s.used.store(false, std::memory_order_release);
  }

  void
  watch_busy(int slot) {
    if (slot >= 0 && slot < max_slots) g.slots[slot].busy_since_ms.store(now_ms(), std::memory_order_release);
  }

  void
  watch_idle(int slot) {
    if (slot >= 0 && slot < max_slots) g.slots[slot].busy_since_ms.store(0, std::memory_order_release);
  }

  std::string
  endpoint_from(const std::string &conf_text) {
    if (const char *env = std::getenv("SUNSHINE_TELEMETRY_ENDPOINT"); env && *env) return env;
    if (auto v = conf_value(conf_text, "telemetry_endpoint"); !v.empty()) return v;
    return "https://api.alkaidlab.com";
  }

  std::string
  current_conf_text() {
    std::lock_guard lock(config_mutex);
    return g_conf_text;
  }

  fs::path
  current_config_dir() {
    std::lock_guard lock(config_mutex);
    return g_config_dir;
  }

  void
  stop_watchdog() {
    g_stop.store(true);
    g_wait_cv.notify_all();
    if (g_watchdog.joinable()) g_watchdog.join();
  }

  bool
  write_report(const fs::path &config_dir, const std::string &stem, const std::string &meta_json, const fs::path &dump) {
    const auto dir = pending_dir(config_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!dump.empty()) fs::rename(dump, dir / (stem + ".dmp"), ec);
    if (ec && !dump.empty()) {
      ec.clear();
      fs::copy_file(dump, dir / (stem + ".dmp"), fs::copy_options::overwrite_existing, ec);
    }
    const auto tmp = dir / (stem + ".json.tmp");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << meta_json;
      if (!out) return false;
    }
    fs::rename(tmp, dir / (stem + ".json"), ec);
    trim_pending(dir);
    return !ec;
  }
}  // namespace crash
