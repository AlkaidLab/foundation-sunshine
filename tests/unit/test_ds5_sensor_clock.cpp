/**
 * @file tests/unit/test_ds5_sensor_clock.cpp
 * @brief Integrated gyro angle against truth for USB-shaped and Bluetooth-shaped arrivals.
 */
#include "src/ds5/sensor_clock.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {
  struct sample_t {
    double true_time_s;
    std::int64_t arrival_us;
    double rate_dps;
  };

  constexpr double pi = 3.14159265358979323846;

  /* The pad samples ω(t) = 200 sin(2π 2 t) degrees per second at a fixed rate; the radio and the network
   * deliver each sample late by jitter, and some of them together after a stall. */
  std::vector<sample_t>
  shape(double rate_hz, double jitter_ms, double burst_probability, double burst_ms, unsigned seed, double seconds) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::vector<sample_t> samples;
    double burst_until_s = -1.0;
    for (double t = 0.0; t < seconds; t += 1.0 / rate_hz) {
      double delay_ms = uniform(random) * jitter_ms;
      if (t > burst_until_s && uniform(random) < burst_probability) burst_until_s = t + burst_ms / 1000.0;
      // Samples inside a burst all arrive at its end.
      const double arrival_s = t < burst_until_s ? burst_until_s + 0.0005 * uniform(random) : t + delay_ms / 1000.0;
      samples.push_back({ t, static_cast<std::int64_t>(arrival_s * 1e6), 200.0 * std::sin(2.0 * pi * 2.0 * t) });
    }
    // The link delivers in order: a sample never overtakes the one before it.
    for (std::size_t index = 1; index < samples.size(); ++index) {
      samples[index].arrival_us = std::max(samples[index].arrival_us, samples[index - 1].arrival_us + 100);
    }
    return samples;
  }

  double
  true_angle(double t) {
    // integral of 200 sin(2π 2 t) dt
    return 200.0 / (2.0 * pi * 2.0) * (1.0 - std::cos(2.0 * pi * 2.0 * t));
  }

  struct error_t {
    double max_deg;  ///< worst error of the angle turned within any half second
    double final_deg;
  };

  /* The game's integration: angle += rate * (stamp - previous stamp). A game measures a turn over a short
   * window, so the error is the one of the angle turned within a half second against the true turn,
   * which does not depend on where the integration started. */
  template <typename Stamp>
  error_t
  integrate(const std::vector<sample_t> &samples, Stamp stamp) {
    std::vector<double> estimated(samples.size(), 0.0);
    double angle = 0.0, previous_stamp = -1.0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
      const double now = stamp(samples[index].arrival_us) / 1e6;
      if (previous_stamp >= 0.0) angle += samples[index].rate_dps * (now - previous_stamp);
      previous_stamp = now;
      estimated[index] = angle;
    }
    double worst = 0.0;
    std::size_t start = 0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
      if (samples[index].true_time_s < 1.5) continue;  // the clock needs a moment to learn the rate
      while (samples[index].true_time_s - samples[start].true_time_s > 0.5) ++start;
      const double turned = estimated[index] - estimated[start];
      const double truth = true_angle(samples[index].true_time_s) - true_angle(samples[start].true_time_s);
      worst = std::max(worst, std::fabs(turned - truth));
    }
    return { worst, estimated.back() - true_angle(samples.back().true_time_s) };
  }

  TEST(Ds5SensorClock, StampsAreEvenlySpacedAndNeverAfterTheArrival) {
    ds5::sensor_clock_t clock;
    const auto samples = shape(128.0, 12.0, 0.01, 40.0, 7, 10.0);
    std::int64_t previous = 0;
    std::vector<double> gaps;
    for (const auto &sample : samples) {
      const auto stamp = clock.stamp(sample.arrival_us);
      EXPECT_LE(stamp, sample.arrival_us);
      EXPECT_GE(stamp, sample.arrival_us - ds5::sensor_clock_t::max_lag_us);
      if (previous != 0) {
        EXPECT_GT(stamp, previous);
        gaps.push_back(static_cast<double>(stamp - previous));
      }
      previous = stamp;
    }
    double mean = 0.0;
    for (const auto gap : gaps) mean += gap;
    mean /= static_cast<double>(gaps.size());
    EXPECT_NEAR(mean, 1e6 / 128.0, 150.0);
  }

  TEST(Ds5SensorClock, IntegratedAngleShapeUsbAndBluetooth) {
    struct shape_t {
      const char *name;
      double rate, jitter, burst_probability, burst_ms;
    } shapes[] = {
      { "usb 250 Hz, 0.3 ms jitter", 250.0, 0.3, 0.0, 0.0 },
      { "bluetooth 128 Hz, 12 ms jitter", 128.0, 12.0, 0.0, 0.0 },
      { "bluetooth 128 Hz, 12 ms jitter + 40 ms bursts", 128.0, 12.0, 0.01, 40.0 },
    };
    for (const auto &candidate : shapes) {
      double raw_worst = 0.0, smooth_worst = 0.0;
      for (unsigned seed = 1; seed <= 20; ++seed) {
        const auto samples = shape(candidate.rate, candidate.jitter, candidate.burst_probability, candidate.burst_ms, seed, 20.0);
        const auto raw = integrate(samples, [](std::int64_t arrival) { return arrival; });
        ds5::sensor_clock_t clock;
        const auto smooth = integrate(samples, [&](std::int64_t arrival) { return clock.stamp(arrival); });
        raw_worst = std::max(raw_worst, raw.max_deg);
        smooth_worst = std::max(smooth_worst, smooth.max_deg);
      }
      std::printf("ANGLE_ERROR %-48s arrival-stamped max=%.2f deg   even-grid max=%.2f deg   (a 200 dps, 2 Hz swing turns up to 63.7 deg in half a second)\n", candidate.name,
        raw_worst, smooth_worst);
      // Never worse than the raw arrival stamps, and within a degree where the arrivals are reasonable.
      EXPECT_LE(smooth_worst, raw_worst + 0.01) << candidate.name;
      if (candidate.burst_probability == 0.0) {
        EXPECT_LT(smooth_worst, 1.0) << candidate.name;
      }
    }
  }

  TEST(Ds5SensorClock, AStallStartsTheGridAgain) {
    ds5::sensor_clock_t clock;
    for (int i = 0; i < 20; ++i) clock.stamp(1000000 + i * 8000);
    EXPECT_EQ(clock.stamp(5000000), 5000000);
    EXPECT_EQ(clock.stamp(5008000), 5008000);
  }
}  // namespace
