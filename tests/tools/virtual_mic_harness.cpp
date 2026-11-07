/**
 * @file tests/tools/virtual_mic_harness.cpp
 * @brief Drives the production virtual microphone session for the verification runs.
 *
 * Modes (all print machine readable "key=value" lines):
 *   hold <seconds>                start the device and keep it idle (prints CPU)
 *   baseline <seconds>            system CPU without the device
 *   run <seconds> [hz] [block_ms] start, write a tone in real time, print the device statistics
 *   cycle <count>                 attach and detach <count> times and check the hub is empty each time
 *   latency <seconds>             write click marks and measure queue-to-capture delay on the endpoint
 *   sweep                         start and stop once (recovers a stale import left by a crash)
 *   defaults                      print the default audio endpoint ids
 *   selftest                      the self-test the Web UI runs (tone in, tone heard)
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <boost/log/sources/severity_logger.hpp>

#include "src/platform/windows/audio_default_guard.h"
#include "src/platform/windows/capture_endpoint_recorder.h"
#include "src/platform/windows/virtual_microphone.h"
#include "src/platform/windows/virtual_microphone_selftest.h"

/* The severity loggers the host sources log through; the harness has no logging module. */
boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

using namespace std::chrono_literals;
using platf::audio::capture_endpoint_recorder;
using platf::audio::virtual_microphone_t;

namespace {

  double
  qpc_100ns() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) * 1e7 / static_cast<double>(frequency.QuadPart);
  }

  double
  process_cpu_seconds() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    auto to_s = [](FILETIME t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7; };
    return to_s(kernel) + to_s(user);
  }

  struct system_times {
    std::uint64_t idle, kernel, user;
  };
  system_times
  read_system_times() {
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    auto to_u = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return { to_u(idle), to_u(kernel), to_u(user) };
  }
  double
  system_busy_percent(const system_times &a, const system_times &b) {
    const double total = static_cast<double>((b.kernel - a.kernel) + (b.user - a.user));  // kernel includes idle
    const double idle = static_cast<double>(b.idle - a.idle);
    return total > 0 ? 100.0 * (total - idle) / total : 0.0;
  }

  int
  imported_ports() {
    FILE *pipe = _popen("usbip.exe port 2>&1", "r");
    if (!pipe) return -1;
    char line[512];
    int count = 0;
    while (std::fgets(line, sizeof line, pipe)) {
      if (std::strncmp(line, "Port ", 5) == 0) ++count;
    }
    _pclose(pipe);
    return count;
  }

  /** Writes a continuous tone in real time, in blocks like a client sends them. */
  class tone_writer {
  public:
    tone_writer(virtual_microphone_t &mic, double hz, int block_ms, double amplitude = 0.4):
        mic_(mic), hz_(hz), block_(48 * block_ms), amplitude_(amplitude) {}
    void
    start() {
      thread_ = std::thread([this] { loop(); });
    }
    void
    stop() {
      stop_ = true;
      if (thread_.joinable()) thread_.join();
    }
    std::uint64_t
    frames() const { return index_.load(); }

  private:
    void
    loop() {
      std::vector<std::int16_t> block(block_);
      auto next = std::chrono::steady_clock::now();
      std::uint64_t index = 0;
      while (!stop_.load()) {
        for (auto &v : block) {
          v = static_cast<std::int16_t>(std::lround(std::sin(2.0 * std::numbers::pi * hz_ * static_cast<double>(index++) / 48000.0) * amplitude_ * 32767.0));
        }
        mic_.write_pcm(block.data(), block.size());
        index_ = index;
        next += std::chrono::milliseconds(block_ / 48);
        std::this_thread::sleep_until(next);
      }
    }
    virtual_microphone_t &mic_;
    double hz_;
    int block_;
    double amplitude_;
    std::atomic<bool> stop_ { false };
    std::atomic<std::uint64_t> index_ { 0 };
    std::thread thread_;
  };

  void
  print_status(const char *label, const platf::audio::virtual_microphone_status &s) {
    std::printf("%s state=%s online=%d created=%d streaming=%d generation=%u buffered_bytes=%u underruns=%u dropped=%u submit_errors=%u last_error=%d error_code=%s\n",
      label, s.state.c_str(), s.online, s.device_created, s.host_streaming, s.generation, s.buffered_bytes, s.underruns,
      s.dropped_frames, s.submit_errors, s.last_error, s.error_code.c_str());
    std::fflush(stdout);
  }

  void
  print_model(const virtual_microphone_t &mic) {
    const auto m = mic.model_stats();
    std::printf("model written=%llu idle_discarded=%llu dropped=%llu trimmed=%llu served=%llu underrun_frames=%llu packets=%llu fill_avg=%.1f ratio=%.6f buffered=%u muted=%d volume=%d\n",
      static_cast<unsigned long long>(m.frames_written), static_cast<unsigned long long>(m.frames_idle_discarded),
      static_cast<unsigned long long>(m.frames_dropped), static_cast<unsigned long long>(m.frames_trimmed),
      static_cast<unsigned long long>(m.frames_served), static_cast<unsigned long long>(m.underrun_frames),
      static_cast<unsigned long long>(m.packets_served), m.fill_average, m.ratio, m.buffered_frames, m.muted, m.volume);
    std::fflush(stdout);
  }

  int
  mode_hold(int seconds) {
    virtual_microphone_t mic;
    const auto error = mic.start();
    std::printf("start result=%s pid=%lu\n", error.empty() ? "ok" : error.c_str(), GetCurrentProcessId());
    std::fflush(stdout);
    if (!error.empty()) return 2;
    const auto cpu0 = process_cpu_seconds();
    const auto sys0 = read_system_times();
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
      std::this_thread::sleep_for(1s);
      print_status("status", mic.status());
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("summary wall_s=%.3f process_cpu_percent_of_one_core=%.3f system_busy_percent=%.3f\n", wall,
      100.0 * (process_cpu_seconds() - cpu0) / wall, system_busy_percent(sys0, read_system_times()));
    mic.stop();
    return 0;
  }

  int
  mode_baseline(int seconds) {
    const auto sys0 = read_system_times();
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("summary wall_s=%.3f system_busy_percent=%.3f\n", wall, system_busy_percent(sys0, read_system_times()));
    return 0;
  }

  int
  mode_run(int seconds, double hz, int block_ms) {
    virtual_microphone_t mic;
    const auto start_at = std::chrono::steady_clock::now();
    const auto error = mic.start();
    std::printf("start result=%s start_ms=%lld pid=%lu\n", error.empty() ? "ok" : error.c_str(),
      static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_at).count()),
      GetCurrentProcessId());
    std::fflush(stdout);
    if (!error.empty()) return 2;
    tone_writer writer(mic, hz, block_ms);
    writer.start();
    const auto cpu0 = process_cpu_seconds();
    const auto sys0 = read_system_times();
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("writing hz=%.1f block_ms=%d\n", hz, block_ms);
    std::fflush(stdout);
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
      std::this_thread::sleep_for(5s);
      print_status("status", mic.status());
      print_model(mic);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto cpu1 = process_cpu_seconds();
    const auto sys1 = read_system_times();
    writer.stop();
    std::printf("summary wall_s=%.3f frames_written=%llu process_cpu_percent_of_one_core=%.3f system_busy_percent=%.3f\n", wall,
      static_cast<unsigned long long>(writer.frames()), 100.0 * (cpu1 - cpu0) / wall, system_busy_percent(sys0, sys1));
    print_status("final", mic.status());
    print_model(mic);
    mic.stop();
    return 0;
  }

  int
  mode_cycle(int count) {
    int failures = 0;
    std::string first_endpoint;
    for (int i = 1; i <= count; ++i) {
      virtual_microphone_t mic;
      const auto t0 = std::chrono::steady_clock::now();
      const auto error = mic.start();
      const auto started = std::chrono::steady_clock::now();
      bool endpoint = false;
      if (error.empty()) {
        capture_endpoint_recorder recorder;
        endpoint = recorder.open(L"Sunshine Virtual Microphone", 20s);
      }
      const auto seen = std::chrono::steady_clock::now();
      const auto stopping = std::chrono::steady_clock::now();
      mic.stop();
      const auto stopped = std::chrono::steady_clock::now();
      std::this_thread::sleep_for(300ms);
      const int ports = imported_ports();
      auto ms = [](auto a, auto b) { return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count()); };
      std::printf("cycle=%d start=%s attach_ms=%lld endpoint_visible=%d endpoint_ms=%lld stop_ms=%lld ports_after_stop=%d\n", i,
        error.empty() ? "ok" : error.c_str(), ms(t0, started), endpoint, ms(t0, seen), ms(stopping, stopped), ports);
      std::fflush(stdout);
      if (!error.empty() || !endpoint || ports != 0) ++failures;
    }
    std::printf("cycles=%d failures=%d\n", count, failures);
    return failures == 0 ? 0 : 1;
  }

  int
  mode_latency(int seconds) {
    virtual_microphone_t mic;
    if (const auto error = mic.start(); !error.empty()) {
      std::printf("start result=%s\n", error.c_str());
      return 2;
    }
    capture_endpoint_recorder recorder;
    if (!recorder.open(L"Sunshine Virtual Microphone", 20s)) {
      std::printf("endpoint not found\n");
      return 3;
    }
    const double rate = recorder.sample_rate();
    std::vector<double> click_write_times;  // QPC 100 ns at the write_pcm call that carried the click
    std::atomic<bool> stop { false };
    std::thread writer([&] {
      std::vector<std::int16_t> block(480, 0);
      auto next = std::chrono::steady_clock::now();
      int tick = 0;
      while (!stop.load()) {
        std::fill(block.begin(), block.end(), std::int16_t { 0 });
        const bool click = tick % 20 == 10;  // every 200 ms, in the middle of the block
        if (click) {
          for (int i = 240; i < 264; ++i) block[i] = 26000;  // 0.5 ms burst
          click_write_times.push_back(qpc_100ns());
        }
        mic.write_pcm(block.data(), block.size());
        ++tick;
        next += 10ms;
        std::this_thread::sleep_until(next);
      }
    });
    std::vector<capture_endpoint_recorder::block> blocks;
    recorder.record(std::chrono::seconds(seconds), blocks);
    stop = true;
    writer.join();

    /* A click was queued at the latest write time that is not after the moment it was seen: the
     * writes are 200 ms apart and the delay is far below that, so the pairing is unambiguous. */
    std::vector<double> stamped, delivered;
    std::size_t clicks_seen = 0;
    float previous = 0;
    auto latest_write_before = [&](double when) {
      double best = -1;
      for (const double w : click_write_times) {
        if (w <= when) best = w;
      }
      return best;
    };
    for (const auto &b : blocks) {
      for (std::size_t i = 0; i < b.samples.size(); ++i) {
        const float v = b.samples[i];
        if (v > 0.3f && previous < 0.1f) {
          ++clicks_seen;
          const double read_at = static_cast<double>(b.read_qpc_100ns);
          const double written = latest_write_before(read_at);
          if (written > 0) delivered.push_back((read_at - written) / 1e4);
          if (b.qpc_100ns != 0) {
            const double at = static_cast<double>(b.qpc_100ns) + static_cast<double>(i) * 1e7 / rate;
            const double written_stamped = latest_write_before(at);
            if (written_stamped > 0) stamped.push_back((at - written_stamped) / 1e4);
          }
        }
        previous = v;
      }
    }
    auto percentile = [](std::vector<double> v, double p) {
      if (v.empty()) return 0.0;
      std::sort(v.begin(), v.end());
      return v[std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1) + 0.5))];
    };
    std::printf("clicks_written=%zu clicks_detected=%zu\n", click_write_times.size(), clicks_seen);
    std::printf("latency_stamped_ms n=%zu p50=%.2f p95=%.2f p99=%.2f min=%.2f max=%.2f\n", stamped.size(),
      percentile(stamped, 0.5), percentile(stamped, 0.95), percentile(stamped, 0.99), percentile(stamped, 0.0), percentile(stamped, 1.0));
    std::printf("latency_delivered_ms n=%zu p50=%.2f p95=%.2f p99=%.2f min=%.2f max=%.2f\n", delivered.size(),
      percentile(delivered, 0.5), percentile(delivered, 0.95), percentile(delivered, 0.99), percentile(delivered, 0.0), percentile(delivered, 1.0));
    print_status("final", mic.status());
    mic.stop();
    return 0;
  }

  int
  mode_sweep() {
    virtual_microphone_t mic;
    const auto error = mic.start();
    std::printf("start result=%s ports_while_running=%d\n", error.empty() ? "ok" : error.c_str(), imported_ports());
    mic.stop();
    std::this_thread::sleep_for(300ms);
    std::printf("ports_after_stop=%d\n", imported_ports());
    return error.empty() ? 0 : 2;
  }

  int
  mode_selftest() {
    const auto result = platf::audio::run_virtual_microphone_selftest();
    std::printf("selftest success=%d error_code=%s heard_amplitude=%.4f written_amplitude=0.18 windows_good=%d/%d\n", result.success,
      result.error_code.c_str(), result.heard_amplitude, result.windows_good, result.windows_total);
    return result.success ? 0 : 1;
  }

  /** What happens in the first seconds after the endpoint appears: a timeline per 250 ms. */
  int
  mode_startup(int seconds) {
    virtual_microphone_t mic;
    if (const auto error = mic.start(); !error.empty()) {
      std::printf("start result=%s\n", error.c_str());
      return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    tone_writer writer(mic, 440, 10, 0.18);
    writer.start();
    capture_endpoint_recorder recorder;
    const bool opened = recorder.open(L"Sunshine Virtual Microphone", 20s);
    std::printf("endpoint opened=%d at %.2f s\n", opened, since());
    if (!opened) return 3;
    for (int chunk = 0; chunk < seconds * 4; ++chunk) {
      std::vector<capture_endpoint_recorder::block> blocks;
      recorder.record(250ms, blocks);
      double energy = 0;
      std::size_t frames = 0, silent = 0, discontinuities = 0;
      for (const auto &b : blocks) {
        for (const float v : b.samples) energy += static_cast<double>(v) * v;
        frames += b.samples.size();
        silent += b.silent ? 1 : 0;
        discontinuities += b.discontinuity ? 1 : 0;
      }
      const auto m = mic.model_stats();
      std::printf("t=%.2f frames=%zu rms=%.4f silent_blocks=%zu discont=%zu | capturing=%d served=%llu buffered=%u underrun_frames=%llu idle_discarded=%llu\n", since(), frames,
        frames ? std::sqrt(energy / static_cast<double>(frames)) : 0.0, silent, discontinuities, m.capturing,
        static_cast<unsigned long long>(m.frames_served), m.buffered_frames, static_cast<unsigned long long>(m.underrun_frames),
        static_cast<unsigned long long>(m.frames_idle_discarded));
      std::fflush(stdout);
    }
    writer.stop();
    return 0;
  }

  int
  mode_defaults() {
    const auto defaults = platf::audio::capture_default_endpoints();
    if (!defaults) return 2;
    for (const auto &d : *defaults) {
      std::wprintf(L"default flow=%ls role=%d id=%ls\n", d.flow == eRender ? L"render" : L"capture", static_cast<int>(d.role), d.id.c_str());
    }
    return 0;
  }

}  // namespace

int
main(int argc, char **argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  /* The host raises the timer resolution the same way; without it a 10 ms writer jitters by 15 ms. */
  timeBeginPeriod(1);
  const std::string mode = argc > 1 ? argv[1] : "";
  auto number = [&](int index, double fallback) { return argc > index ? std::atof(argv[index]) : fallback; };
  if (mode == "baseline") return mode_baseline(static_cast<int>(number(2, 30)));
  if (mode == "hold") return mode_hold(static_cast<int>(number(2, 30)));
  if (mode == "run") return mode_run(static_cast<int>(number(2, 30)), number(3, 1000), static_cast<int>(number(4, 10)));
  if (mode == "cycle") return mode_cycle(static_cast<int>(number(2, 20)));
  if (mode == "latency") return mode_latency(static_cast<int>(number(2, 30)));
  if (mode == "sweep") return mode_sweep();
  if (mode == "defaults") return mode_defaults();
  if (mode == "startup") return mode_startup(static_cast<int>(number(2, 12)));
  if (mode == "selftest") return mode_selftest();
  std::fprintf(stderr, "usage: virtual_mic_harness hold|run|cycle|latency|sweep|defaults ...\n");
  return 64;
}
