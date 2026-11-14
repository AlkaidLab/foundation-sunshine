/**
 * @file src/crash/crash_posix.cpp
 * @brief Linux/POSIX crash capture, best effort: fatal-signal and terminate handlers that write the meta (frames only, no dump).
 *
 * The handler runs on its own signal stack, formats with fixed buffers and writes with open/write/rename (async-signal-safe); the module of
 * each frame comes from a /proc/self/maps snapshot taken at start and refreshed every 30 s, so nothing is read or allocated in the handler.
 */
#ifndef _WIN32

  #include "crash_state.h"

  #include <csignal>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <exception>
  #include <execinfo.h>
  #include <fcntl.h>
  #include <fstream>
  #include <stdexcept>
  #include <string_view>
  #include <sys/stat.h>
  #include <thread>
  #include <unistd.h>

namespace crash::detail {
  namespace {
    constexpr int max_modules = 384;
    struct mod_t {
      std::uint64_t start = 0, end = 0, base = 0;
      char name[module_name_size] = {};
    };
    struct snapshot_t {
      mod_t m[max_modules];
      int n = 0;
    };
    snapshot_t g_snap[2];
    std::atomic<int> g_snap_index { 0 };
    volatile sig_atomic_t g_crashing = 0;
    int g_tick = 0;
    void *g_stack = nullptr;

    void
    refresh_modules() {
      const int next = 1 - g_snap_index.load();
      auto &snap = g_snap[next];
      snap.n = 0;
      std::ifstream maps("/proc/self/maps");
      std::string line;
      while (std::getline(maps, line) && snap.n < max_modules) {
        unsigned long long start = 0, end = 0, pgoff = 0;
        char perms[8] = {}, path[512] = {};
        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %511[^\n]", &start, &end, perms, &pgoff, path) < 4) continue;
        if (path[0] != '/') continue;
        const char *file = std::strrchr(path, '/') + 1;
        // the mapping at file offset 0 is the image base; later mappings of the same file extend it
        if (pgoff == 0) {
          auto &m = snap.m[snap.n++];
          m.start = start;
          m.end = end;
          m.base = start;
          std::strncpy(m.name, file, sizeof(m.name) - 1);
          m.name[sizeof(m.name) - 1] = 0;
        }
        else if (snap.n > 0 && std::strncmp(snap.m[snap.n - 1].name, file, sizeof(snap.m[0].name) - 1) == 0) {
          snap.m[snap.n - 1].end = end;
        }
      }
      g_snap_index.store(next);
    }

    void
    resolve(std::uint64_t pc, frame_t &out) {
      const auto &snap = g_snap[g_snap_index.load()];
      for (int i = 0; i < snap.n; ++i) {
        if (pc >= snap.m[i].start && pc < snap.m[i].end) {
          std::memcpy(out.module, snap.m[i].name, sizeof(out.module));
          out.offset = pc - snap.m[i].base;
          return;
        }
      }
      std::memcpy(out.module, "unknown", 8);
      out.offset = pc;
    }

    void
    write_meta_file(const char *kind, const char *code, const frame_t *frames, std::size_t count) {
      char buffer[meta_buffer_size];
      meta_t meta;
      meta.product = g.product;
      meta.version = g.version;
      meta.os_version = g.os_version;
      meta.kind = kind;
      meta.fault_code = code;
      meta.time = now_unix();
      meta.uptime_s = (now_ms() - g.start_ms) / 1000;
      meta.frames = frames;
      meta.frame_count = count;
      const auto len = render_meta(meta, buffer, sizeof(buffer));
      if (!len) return;
      char stem[112];
      stem_noalloc(stem, sizeof(stem), now_unix(), kind, (std::uint32_t) getpid());
      char path[path_cap + 160], tmp[path_cap + 170];
      std::snprintf(path, sizeof(path), "%s/%s.json", g.pending_utf8, stem);
      std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) return;
      const auto written = write(fd, buffer, len);
      close(fd);
      if (written == (ssize_t) len) rename(tmp, path);
    }

    void
    capture_and_write(const char *kind, const char *code, int skip) {
      if (!g.enabled.load()) return;
      void *raw[max_frames + 8];
      int n = backtrace(raw, (int) (max_frames + 8));
      frame_t frames[max_frames];
      std::size_t count = 0;
      for (int i = skip; i < n && count < max_frames; ++i) resolve((std::uint64_t) (std::uintptr_t) raw[i], frames[count++]);
      if (!count) {
        std::memcpy(frames[0].module, "unknown", 8);
        count = 1;
      }
      write_meta_file(kind, code, frames, count);
    }

    const char *
    signal_name(int sig) {
      switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        default: return "SIGNAL";
      }
    }

    void
    on_signal(int sig, siginfo_t *, void *) {
      if (g_crashing) _exit(128 + sig);
      g_crashing = 1;
      capture_and_write(sig == SIGABRT ? "abort" : "crash", signal_name(sig), 2);
      std::signal(sig, SIG_DFL);
      raise(sig);
    }

    [[noreturn]] void
    on_terminate() {
      if (!g_crashing) {
        g_crashing = 1;
        capture_and_write("abort", "terminate", 2);
      }
      std::signal(SIGABRT, SIG_DFL);
      std::abort();
    }
  }  // namespace

  void
  platform_install() {
    refresh_modules();
    void *warm[4];
    backtrace(warm, 4);  // loads libgcc now, not inside a handler
    g_stack = std::malloc(256 * 1024);
    stack_t ss {};
    ss.ss_sp = g_stack;
    ss.ss_size = 256 * 1024;
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) sigaction(sig, &sa, nullptr);
    std::set_terminate(on_terminate);
  }

  void
  platform_tick() {
    if (++g_tick % 30 == 0) refresh_modules();
  }

  void
  platform_bind_thread(slot_t &) {}

  void
  platform_release_thread(slot_t &) {}

  void
  platform_report_hang(int index) {
    // another thread's stack is not readable from here without stopping it; the slot's name stands in for the frames
    frame_t frame;
    std::strncpy(frame.module, "hang-", sizeof(frame.module) - 1);
    std::strncat(frame.module, g.slots[index].name, sizeof(frame.module) - std::strlen(frame.module) - 1);
    char code[48] = "hang:";
    std::strncat(code, g.slots[index].name, sizeof(code) - 6);
    write_meta_file("hang", code, &frame, 1);
  }

  void
  platform_test_crash(const char *what) {
    const std::string_view mode { what };
    if (mode == "null-deref") {
      volatile int *p = nullptr;
      *p = 42;
    }
    else if (mode == "abort") {
      std::abort();
    }
    else if (mode == "terminate") {
      std::terminate();
    }
    else if (mode == "throw") {
      std::thread([] { throw std::runtime_error("SUNSHINE_TEST_CRASH throw"); }).join();
    }
    else if (mode == "hang") {
      const int slot = watch_register("test-hang", 3000);
      watch_busy(slot);
      sleep(9);
      watch_idle(slot);
      watch_release(slot);
      _exit(0);
    }
  }
}  // namespace crash::detail

#endif  // !_WIN32
