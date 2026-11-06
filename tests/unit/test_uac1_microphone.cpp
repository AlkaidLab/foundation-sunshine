/**
 * @file tests/unit/test_uac1_microphone.cpp
 * @brief Descriptor, control request and data path tests of the virtual microphone model.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "src/virtual_usb/uac1_microphone.h"

using namespace virtual_usb;

namespace {

  setup_packet
  setup(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
    return { type, request, value, index, length };
  }

  /** What Windows does when an application starts recording: select configuration 1, alternate 1. */
  void
  start_stream(uac1_microphone &mic) {
    ASSERT_FALSE(mic.control(setup(0x00, 9, 1, 0, 0), {}).stall);
    ASSERT_FALSE(mic.control(setup(0x01, 11, 1, 1, 0), {}).stall);
  }

  std::vector<std::int16_t>
  serve(uac1_microphone &mic, std::size_t packets) {
    std::vector<std::uint32_t> room(packets, uac1_microphone::kMaxPacketBytes);
    iso_in_reply reply;
    EXPECT_TRUE(mic.iso_in({ uac1_microphone::kStreamingEndpoint, room }, reply));
    EXPECT_EQ(reply.actual_length.size(), packets);
    std::vector<std::int16_t> out(reply.payload.size() / 2);
    std::memcpy(out.data(), reply.payload.data(), reply.payload.size());
    return out;
  }

  double
  goertzel_amplitude(const std::vector<std::int16_t> &x, double frequency) {
    const double omega = 2.0 * std::numbers::pi * frequency / 48000.0;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0, s2 = 0;
    for (auto v : x) {
      const double s = v + coeff * s1 - s2;
      s2 = s1;
      s1 = s;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(power) / static_cast<double>(x.size());
  }

  /** Write `count` frames of a tone that starts at `*phase` samples. */
  void
  write_tone(uac1_microphone &mic, std::uint64_t &phase, std::size_t count, double hz, double amplitude = 12000) {
    std::vector<std::int16_t> block(count);
    for (std::size_t i = 0; i < count; ++i, ++phase) {
      block[i] = static_cast<std::int16_t>(std::lround(amplitude * std::sin(2.0 * std::numbers::pi * hz * phase / 48000.0)));
    }
    ASSERT_EQ(mic.write_pcm(block.data(), block.size()), block.size());
  }

}  // namespace

TEST(Uac1Descriptors, ConfigurationIsWellFormed) {
  const auto config = uac1_microphone::configuration_descriptor();
  ASSERT_GE(config.size(), 9u);
  EXPECT_EQ(config[2] | (config[3] << 8), static_cast<int>(config.size()));
  EXPECT_EQ(config[4], 2);  // AudioControl plus one AudioStreaming interface

  std::size_t offset = 0;
  int endpoints = 0;
  int alt1_formats = 0;
  while (offset < config.size()) {
    const auto length = config[offset];
    ASSERT_GE(length, 2u);
    ASSERT_LE(offset + length, config.size());
    if (config[offset + 1] == 5) {
      ++endpoints;
      EXPECT_EQ(config[offset + 2], 0x81);  // IN, endpoint 1
      EXPECT_EQ(config[offset + 3], 0x05);  // isochronous, asynchronous
      EXPECT_EQ(config[offset + 4] | (config[offset + 5] << 8), 98);  // 96 plus one frame of slack
      EXPECT_EQ(config[offset + 6], 4);  // 1 ms at high speed
    }
    if (config[offset + 1] == 0x24 && config[offset + 2] == 0x02 && length == 11) {
      ++alt1_formats;
      EXPECT_EQ(config[offset + 4], 1);  // mono
      EXPECT_EQ(config[offset + 5], 2);  // two bytes per sample
      EXPECT_EQ(config[offset + 6], 16);  // 16 bits
      EXPECT_EQ(config[offset + 8] | (config[offset + 9] << 8) | (config[offset + 10] << 16), 48000);
    }
    offset += length;
  }
  EXPECT_EQ(offset, config.size());
  EXPECT_EQ(endpoints, 1);
  EXPECT_EQ(alt1_formats, 1);
}

TEST(Uac1Descriptors, FeatureUnitOffersMasterMuteAndVolume) {
  const auto config = uac1_microphone::configuration_descriptor();
  bool found = false;
  for (std::size_t offset = 0; offset < config.size(); offset += config[offset]) {
    if (config[offset + 1] == 0x24 && config[offset + 2] == 0x06) {
      found = true;
      EXPECT_EQ(config[offset], 9);  // 7 + (1 channel + master) * 1 byte
      EXPECT_EQ(config[offset + 3], uac1_microphone::kFeatureUnitId);
      EXPECT_EQ(config[offset + 5], 1);  // bControlSize
      EXPECT_EQ(config[offset + 6], 0x03);  // master: mute and volume
    }
  }
  EXPECT_TRUE(found);
}

TEST(Uac1Descriptors, DeviceAndStrings) {
  const auto device = uac1_microphone::device_descriptor();
  ASSERT_EQ(device.size(), 18u);
  EXPECT_EQ(device[0], 18);
  EXPECT_EQ(device[8] | (device[9] << 8), kVirtualMicrophoneVendorId);
  EXPECT_EQ(device[10] | (device[11] << 8), kVirtualMicrophoneProductId);
  EXPECT_NE(kVirtualMicrophoneVendorId, 0x054c);  // never the controller's identity

  const auto product = uac1_microphone::string_descriptor(2);
  ASSERT_EQ(product[0], product.size());
  std::string text;
  for (std::size_t i = 2; i + 1 < product.size(); i += 2) text.push_back(static_cast<char>(product[i]));
  EXPECT_EQ(text, "Sunshine Virtual Microphone");
  EXPECT_TRUE(uac1_microphone::string_descriptor(9).empty());

  const auto other = uac1_microphone::configuration_descriptor(7);
  EXPECT_EQ(other[1], 7);
  EXPECT_EQ(other.size(), uac1_microphone::configuration_descriptor().size());
}

TEST(Uac1Control, DescriptorRequestsAreTruncatedToWLength) {
  uac1_microphone mic;
  auto result = mic.control(setup(0x80, 6, 0x0200, 0, 9), {});
  ASSERT_FALSE(result.stall);
  EXPECT_EQ(result.data.size(), 9u);
  result = mic.control(setup(0x80, 6, 0x0200, 0, 255), {});
  EXPECT_EQ(result.data, uac1_microphone::configuration_descriptor());
  result = mic.control(setup(0x80, 6, 0x0100, 0, 18), {});
  EXPECT_EQ(result.data, uac1_microphone::device_descriptor());
  EXPECT_TRUE(mic.control(setup(0x80, 6, 0x0305, 0x0409, 255), {}).stall);  // no such string
  EXPECT_TRUE(mic.control(setup(0x80, 6, 0x0201, 0, 255), {}).stall);  // no second configuration
  EXPECT_TRUE(mic.control(setup(0xc0, 0x77, 0, 0, 4), {}).stall);  // unknown vendor request
}

TEST(Uac1Control, ConfigurationAndInterfaceSelection) {
  uac1_microphone mic;
  EXPECT_FALSE(mic.control(setup(0x00, 9, 1, 0, 0), {}).stall);
  EXPECT_EQ(mic.control(setup(0x80, 8, 0, 0, 1), {}).data, std::vector<std::uint8_t> { 1 });
  EXPECT_FALSE(mic.stats().capturing);
  EXPECT_FALSE(mic.control(setup(0x01, 11, 1, 1, 0), {}).stall);
  EXPECT_TRUE(mic.stats().capturing);
  EXPECT_EQ(mic.control(setup(0x81, 10, 0, 1, 1), {}).data, std::vector<std::uint8_t> { 1 });
  EXPECT_TRUE(mic.control(setup(0x01, 11, 2, 1, 0), {}).stall);  // no alternate 2
  EXPECT_TRUE(mic.control(setup(0x01, 11, 1, 7, 0), {}).stall);  // no interface 7
  mic.bus_reset();
  EXPECT_FALSE(mic.stats().capturing);
}

TEST(Uac1Control, MuteAndVolumeFollowTheClassRequests) {
  uac1_microphone mic;
  const std::uint16_t unit = static_cast<std::uint16_t>(uac1_microphone::kFeatureUnitId << 8);
  const std::uint8_t out = 0x21, in = 0xa1;

  EXPECT_EQ(mic.control(setup(in, 0x81, 0x0100, unit, 1), {}).data, std::vector<std::uint8_t> { 0 });
  const std::uint8_t on = 1;
  EXPECT_FALSE(mic.control(setup(out, 0x01, 0x0100, unit, 1), std::span(&on, 1)).stall);
  EXPECT_EQ(mic.control(setup(in, 0x81, 0x0100, unit, 1), {}).data, std::vector<std::uint8_t> { 1 });

  EXPECT_EQ(mic.control(setup(in, 0x82, 0x0200, unit, 2), {}).data, (std::vector<std::uint8_t> { 0x00, 0xce }));  // -12800
  EXPECT_EQ(mic.control(setup(in, 0x83, 0x0200, unit, 2), {}).data, (std::vector<std::uint8_t> { 0x00, 0x00 }));
  EXPECT_EQ(mic.control(setup(in, 0x84, 0x0200, unit, 2), {}).data, (std::vector<std::uint8_t> { 0x00, 0x01 }));
  const std::uint8_t minus_six_db[] = { 0x00, 0xfa };  // -1536 = -6 dB
  EXPECT_FALSE(mic.control(setup(out, 0x01, 0x0200, unit, 2), minus_six_db).stall);
  EXPECT_EQ(mic.control(setup(in, 0x81, 0x0200, unit, 2), {}).data, (std::vector<std::uint8_t> { 0x00, 0xfa }));
  const std::uint8_t too_loud[] = { 0x00, 0x01 };
  EXPECT_TRUE(mic.control(setup(out, 0x01, 0x0200, unit, 2), too_loud).stall);
  EXPECT_TRUE(mic.control(setup(in, 0x81, 0x0100, unit, 1), {}).data.size() == 1);
  // Channel 1 has no controls of its own.
  EXPECT_TRUE(mic.control(setup(in, 0x81, 0x0101, unit, 1), {}).stall);
}

TEST(Uac1DataPath, PlaysSilenceBeforeAnyAudioAndNeverBlocks) {
  uac1_microphone mic;
  const auto pcm = serve(mic, 10);
  ASSERT_EQ(pcm.size(), 480u);
  for (auto v : pcm) EXPECT_EQ(v, 0);
  const auto s = mic.stats();
  EXPECT_EQ(s.underruns, 0u);  // nobody is talking: not an underrun
  EXPECT_EQ(s.underrun_frames, 0u);
  EXPECT_EQ(s.packets_served, 10u);
}

TEST(Uac1DataPath, PassesSamplesThroughBitExactInsideTheDeadBand) {
  microphone_tuning tuning;
  tuning.dead_band_frames = 4000;  // the fill never leaves the band, so the servo stays out
  uac1_microphone mic(tuning);
  start_stream(mic);
  std::vector<std::int16_t> input(2000);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::int16_t>(i % 20000 + 1);
  ASSERT_EQ(mic.write_pcm(input.data(), input.size()), input.size());
  const auto out = serve(mic, 30);
  // Find the first non-zero sample, then the stream must continue unchanged.
  std::size_t first = 0;
  while (first < out.size() && out[first] == 0) ++first;
  ASSERT_LT(first, out.size());
  EXPECT_EQ(out[first], 1);
  for (std::size_t i = first; i < out.size() && i - first < input.size(); ++i) {
    ASSERT_EQ(out[i], input[i - first]) << "at " << i;
  }
}

TEST(Uac1DataPath, NothingIsQueuedWhileNoApplicationRecords) {
  uac1_microphone mic;
  std::vector<std::int16_t> block(960, 5);
  EXPECT_EQ(mic.write_pcm(block.data(), block.size()), block.size());
  auto s = mic.stats();
  EXPECT_EQ(s.frames_idle_discarded, 960u);
  EXPECT_EQ(s.buffered_frames, 0u);
  EXPECT_EQ(s.frames_dropped, 0u);
  EXPECT_TRUE(s.remote_active);
  start_stream(mic);
  EXPECT_EQ(mic.write_pcm(block.data(), block.size()), block.size());
  EXPECT_EQ(mic.stats().buffered_frames, 960u);
}

TEST(Uac1DataPath, FullRingDropsIncomingFramesAndCountsThem) {
  uac1_microphone mic;
  start_stream(mic);
  std::vector<std::int16_t> block(5000, 100);
  EXPECT_EQ(mic.write_pcm(block.data(), block.size()), block.size());
  const auto stored = mic.write_pcm(block.data(), block.size());
  EXPECT_EQ(stored, 8192u - 5000u);
  const auto s = mic.stats();
  EXPECT_EQ(s.frames_written, 8192u);
  EXPECT_EQ(s.frames_dropped, 10000u - 8192u);
  EXPECT_EQ(s.buffered_frames, 8192u);
}

TEST(Uac1DataPath, BacklogIsTrimmedToTheTargetNotPlayedLate) {
  uac1_microphone mic;
  start_stream(mic);
  std::vector<std::int16_t> block(6000, 7);
  ASSERT_EQ(mic.write_pcm(block.data(), block.size()), block.size());
  serve(mic, 1);
  const auto s = mic.stats();
  EXPECT_GT(s.frames_trimmed, 3000u);
  EXPECT_LE(s.buffered_frames, 2880u);
}

TEST(Uac1DataPath, UnderrunGivesSilenceAndRefillsBeforeResuming) {
  uac1_microphone mic;
  start_stream(mic);
  std::uint64_t phase = 0;
  write_tone(mic, phase, 2400, 1000);
  serve(mic, 20);  // running, 960 frames drained
  auto s = mic.stats();
  EXPECT_EQ(s.underruns, 0u);
  serve(mic, 40);  // 1920 more than the 1440 left: runs dry
  s = mic.stats();
  EXPECT_EQ(s.underruns, 1u);
  EXPECT_GT(s.underrun_frames, 0u);
  write_tone(mic, phase, 480, 1000);  // not enough to restart (needs the start level)
  auto pcm = serve(mic, 5);
  for (auto v : pcm) EXPECT_EQ(v, 0);
  write_tone(mic, phase, 2400, 1000);
  pcm = serve(mic, 10);
  EXPECT_NE(std::count_if(pcm.begin(), pcm.end(), [](auto v) { return v != 0; }), 0);
}

TEST(Uac1DataPath, FlushDropsQueuedAudio) {
  uac1_microphone mic;
  start_stream(mic);
  std::uint64_t phase = 0;
  write_tone(mic, phase, 4000, 1000);
  mic.flush();
  const auto pcm = serve(mic, 5);
  for (auto v : pcm) EXPECT_EQ(v, 0);
  EXPECT_EQ(mic.stats().buffered_frames, 0u);
}

TEST(Uac1DataPath, VolumeAndMuteScaleTheOutput) {
  uac1_microphone mic;
  start_stream(mic);
  const std::uint16_t unit = static_cast<std::uint16_t>(uac1_microphone::kFeatureUnitId << 8);
  const std::uint8_t minus_six_db[] = { 0x00, 0xfa };
  ASSERT_FALSE(mic.control(setup(0x21, 0x01, 0x0200, unit, 2), minus_six_db).stall);
  std::uint64_t phase = 0;
  write_tone(mic, phase, 2400, 1000, 16000);
  auto pcm = serve(mic, 40);
  pcm.erase(pcm.begin(), pcm.begin() + 480);
  EXPECT_NEAR(goertzel_amplitude(pcm, 1000), 16000 * 0.501, 400);

  const std::uint8_t on = 1;
  ASSERT_FALSE(mic.control(setup(0x21, 0x01, 0x0100, unit, 1), std::span(&on, 1)).stall);
  write_tone(mic, phase, 2400, 1000, 16000);
  pcm = serve(mic, 40);
  for (auto v : pcm) EXPECT_EQ(v, 0);
}

/** The two clocks differ by 300 ppm for a simulated minute: no underrun, no trim, the tone keeps its pitch. */
TEST(Uac1DataPath, FollowsAFasterAndASlowerSourceClock) {
  for (const double ppm : { 300.0, -300.0, 0.0 }) {
    uac1_microphone mic;
    start_stream(mic);
    std::uint64_t phase = 0;
    double owed = 0;
    std::vector<std::int16_t> all;
    constexpr int seconds = 60;
    for (int ms = 0; ms < seconds * 1000; ++ms) {
      owed += 48.0 * (1.0 + ppm * 1e-6);
      if (ms % 10 == 9) {  // the client delivers 10 ms blocks
        const auto frames = static_cast<std::size_t>(owed);
        owed -= static_cast<double>(frames);
        write_tone(mic, phase, frames, 1000);
      }
      const auto pkt = serve(mic, 1);
      all.insert(all.end(), pkt.begin(), pkt.end());
    }
    const auto s = mic.stats();
    EXPECT_EQ(s.underruns, 0u) << ppm;
    EXPECT_EQ(s.frames_trimmed, 0u) << ppm;
    EXPECT_EQ(s.frames_dropped, 0u) << ppm;
    EXPECT_LT(s.buffered_frames, 2880u) << ppm;
    // The source tone is 1000 Hz on the source clock; on the device clock it is 1000 * (1 - ppm).
    std::vector<std::int16_t> tail(all.end() - 48000, all.end());
    const double expected = 1000.0 * (1.0 + ppm * 1e-6);
    EXPECT_GT(goertzel_amplitude(tail, expected), 11000) << ppm;
  }
}
