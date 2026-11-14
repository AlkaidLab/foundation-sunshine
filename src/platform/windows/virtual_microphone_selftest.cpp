/**
 * @file src/platform/windows/virtual_microphone_selftest.cpp
 * @brief The microphone self-test (see the header).
 */
#include "virtual_microphone_selftest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "src/platform/windows/capture_endpoint_recorder.h"
#include "src/platform/windows/virtual_microphone.h"

namespace platf::audio {

  using namespace std::chrono_literals;

  namespace {
    /** Amplitude of one frequency in `x` (Goertzel). */
    double
    tone_amplitude(const std::vector<float> &x, double frequency, double rate) {
      const double omega = 2.0 * std::numbers::pi * frequency / rate;
      const double coeff = 2.0 * std::cos(omega);
      double s1 = 0, s2 = 0;
      for (const float v : x) {
        const double s = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
      }
      return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double>(x.size());
    }
  }  // namespace

  virtual_microphone_selftest_result
  run_virtual_microphone_selftest() {
    virtual_microphone_selftest_result result;
    virtual_microphone_t mic;
    if (const auto error = mic.start(); !error.empty()) {
      result.error_code = error == "MIC_USBIP_COMPONENT_UNAVAILABLE" ? error : "MIC_TEST_DEVICE_UNAVAILABLE";
      return result;
    }

    capture_endpoint_recorder recorder;
    if (!recorder.open(L"Sunshine Virtual Microphone", 15s)) {
      result.error_code = "MIC_TEST_DEVICE_UNAVAILABLE";
      return result;
    }

    constexpr double tone_hz = 440.0;
    constexpr double amplitude = 0.18;
    std::atomic<bool> stop_writer { false };
    std::thread writer([&] {
      std::vector<std::int16_t> block(480);
      std::uint64_t index = 0;
      auto next = std::chrono::steady_clock::now();
      while (!stop_writer.load()) {
        for (auto &v : block) {
          v = static_cast<std::int16_t>(std::lround(std::sin(2.0 * std::numbers::pi * tone_hz * static_cast<double>(index++) / 48000.0) * amplitude * 32767.0));
        }
        mic.write_pcm(block.data(), block.size());
        next += 10ms;
        std::this_thread::sleep_until(next);
      }
    });
    std::vector<capture_endpoint_recorder::block> blocks;
    /* The engine ramp-up of the freshly attached device gets its own bounded
     * wait (see capture_endpoint_recorder::record); the judged window is the
     * 3.5 s after the first captured packet. */
    const bool recorded = recorder.record(3500ms, blocks, 3500ms);
    stop_writer = true;
    writer.join();

    std::vector<float> heard;
    const auto rate = recorder.sample_rate();
    for (const auto &b : blocks) heard.insert(heard.end(), b.samples.begin(), b.samples.end());
    result.heard_seconds = rate > 0 ? static_cast<double>(heard.size()) / rate : 0.0;
    if (!recorded || rate == 0 || heard.size() < static_cast<std::size_t>(rate) * 2) {
      result.error_code = "MIC_TEST_WRITE_FAILED";
      return result;
    }

    /* Judge 500 ms windows after the first second.  A window is good when the tone is a
     * large part of what was recorded; the test passes when most windows are good, so a
     * short stall of a busy machine does not fail it but a missing or wrong signal does. */
    const std::size_t window = rate / 2;
    for (std::size_t start = rate; start + window <= heard.size(); start += window / 2) {
      const std::vector<float> piece(heard.begin() + static_cast<std::ptrdiff_t>(start), heard.begin() + static_cast<std::ptrdiff_t>(start + window));
      double energy = 0;
      for (const float v : piece) energy += static_cast<double>(v) * v;
      const double rms = std::sqrt(energy / static_cast<double>(piece.size()));
      const double amplitude_found = tone_amplitude(piece, tone_hz, rate);
      ++result.windows_total;
      // A clean tone has an amplitude of sqrt(2) times its RMS.
      if (amplitude_found > amplitude * 0.5 && amplitude_found > rms * 1.414 * 0.8) ++result.windows_good;
      result.heard_amplitude = std::max(result.heard_amplitude, amplitude_found);
    }
    if (result.windows_total > 0 && result.windows_good * 3 >= result.windows_total * 2) {
      result.success = true;
    }
    else {
      result.error_code = "MIC_TEST_WRITE_FAILED";
    }
    return result;
  }

}  // namespace platf::audio
