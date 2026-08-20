/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include <src/audio.h>

#include "../tests_common.h"

using namespace audio;

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void
  SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  auto result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

config_t
make_config(int packet_duration,
            int channels,
            int mask,
            std::bitset<config_t::MAX_FLAGS> flags,
            stream_params_t custom_stream_params = {}) {
  config_t config {};
  config.packetDuration = packet_duration;
  config.channels = channels;
  config.mask = mask;
  config.codec = CODEC_OPUS;
  config.customStreamParams = custom_stream_params;
  config.flags = flags;
  return config;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", make_config(5, 2, 0x3, config_flags(config_t::HIGH_QUALITY))),
    std::make_tuple("SURROUND51", make_config(5, 6, 0x3F, config_flags())),
    std::make_tuple("SURROUND71", make_config(5, 8, 0x63F, config_flags())),
    std::make_tuple("SURROUND51_CUSTOM", make_config(5, 6, 0x3F, config_flags(config_t::CUSTOM_SURROUND_PARAMS), { 6, 4, 2, { 0, 1, 4, 5, 2, 3 } }))),
  [](const auto &info) { return std::string(std::get<0>(info.param)); });

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}
