/**
 * @file tests/unit/test_mic_mixer.cpp
 * @brief Test src/mic_mixer.*.
 */
#include <src/mic_mixer.h>

#include "../tests_common.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <opus/opus.h>

namespace {
  struct opus_encoder_deleter_t {
    void
    operator()(OpusEncoder *encoder) const noexcept {
      if (encoder) {
        opus_encoder_destroy(encoder);
      }
    }
  };

  using opus_encoder_t = std::unique_ptr<OpusEncoder, opus_encoder_deleter_t>;

  opus_encoder_t
  make_encoder() {
    int error = OPUS_OK;
    opus_encoder_t encoder {
      opus_encoder_create(
        static_cast<opus_int32>(mic_mixer::sample_rate),
        1,
        OPUS_APPLICATION_AUDIO,
        &error)
    };
    EXPECT_EQ(error, OPUS_OK);
    EXPECT_TRUE(static_cast<bool>(encoder));
    return encoder;
  }

  std::vector<std::uint8_t>
  encode_frame(OpusEncoder *encoder, std::size_t sample_count, std::int16_t sample_value) {
    if (!encoder) {
      return {};
    }

    std::vector<std::int16_t> pcm(sample_count, sample_value);
    std::vector<std::uint8_t> encoded(4000);
    const auto encoded_size = opus_encode(
      encoder,
      pcm.data(),
      static_cast<int>(pcm.size()),
      encoded.data(),
      static_cast<opus_int32>(encoded.size()));
    EXPECT_GT(encoded_size, 0);
    if (encoded_size <= 0) {
      return {};
    }

    encoded.resize(static_cast<std::size_t>(encoded_size));
    return encoded;
  }

  std::vector<std::int16_t>
  decode_single_frame(const std::vector<std::uint8_t> &packet) {
    mic_mixer::mixer_t mixer;
    EXPECT_TRUE(mixer.add_source(1));
    EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 1));
    auto decoded = mixer.mix_next_frame();
    EXPECT_TRUE(decoded.has_value());
    return decoded.value_or(std::vector<std::int16_t> {});
  }
}  // namespace

TEST(MicMixerTest, AcceptsOnlyTwentyMillisecondOpusFrames) {
  auto encoder = make_encoder();
  const auto twenty_ms = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto ten_ms = encode_frame(encoder.get(), mic_mixer::frame_samples / 2, 1000);

  ASSERT_FALSE(twenty_ms.empty());
  ASSERT_FALSE(ten_ms.empty());
  EXPECT_TRUE(mic_mixer::is_valid_opus_packet(twenty_ms.data(), twenty_ms.size()));
  EXPECT_FALSE(mic_mixer::is_valid_opus_packet(ten_ms.data(), ten_ms.size()));
  EXPECT_FALSE(mic_mixer::is_valid_opus_packet(nullptr, 0));
}

TEST(MicMixerTest, MixesIndependentSourcesIntoOneFrame) {
  auto first_encoder = make_encoder();
  auto second_encoder = make_encoder();
  const auto first_packet = encode_frame(first_encoder.get(), mic_mixer::frame_samples, 6000);
  const auto second_packet = encode_frame(second_encoder.get(), mic_mixer::frame_samples, -2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());

  const auto first_decoded = decode_single_frame(first_packet);
  const auto second_decoded = decode_single_frame(second_packet);
  ASSERT_EQ(first_decoded.size(), mic_mixer::frame_samples);
  ASSERT_EQ(second_decoded.size(), mic_mixer::frame_samples);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.add_source(2));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1));
  ASSERT_TRUE(mixer.push_packet(2, second_packet.data(), second_packet.size(), 1));

  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  ASSERT_EQ(mixed->size(), mic_mixer::frame_samples);
  for (std::size_t sample = 0; sample < mixed->size(); ++sample) {
    const auto expected = (static_cast<std::int64_t>(first_decoded[sample]) +
                           static_cast<std::int64_t>(second_decoded[sample])) /
                          2;
    EXPECT_EQ((*mixed)[sample], expected);
  }
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, KeepsOnlyThreeNewestQueuedFrames) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  for (std::uint16_t sequence = 1; sequence <= 4; ++sequence) {
    ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), sequence));
  }

  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, RejectsDuplicatesAndAcceptsSequenceWraparound) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 0xFFFF));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 0));
  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 0));

  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, SingleLatePacketDoesNotResetSequenceState) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 100));
  ASSERT_TRUE(mixer.mix_next_frame().has_value());

  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 90));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 101));
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, ConsecutiveRollbackPacketsRestartTheSource) {
  auto old_encoder = make_encoder();
  auto restarted_encoder = make_encoder();
  const auto old_packet = encode_frame(old_encoder.get(), mic_mixer::frame_samples, 1000);
  const auto restart_packet = encode_frame(restarted_encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(old_packet.empty());
  ASSERT_FALSE(restart_packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, old_packet.data(), old_packet.size(), 100));
  EXPECT_FALSE(mixer.push_packet(1, restart_packet.data(), restart_packet.size(), 10));
  EXPECT_TRUE(mixer.push_packet(1, restart_packet.data(), restart_packet.size(), 11));

  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, RemovingSourcesDiscardsTheirQueuedFrames) {
  auto first_encoder = make_encoder();
  auto second_encoder = make_encoder();
  const auto first_packet = encode_frame(first_encoder.get(), mic_mixer::frame_samples, 6000);
  const auto second_packet = encode_frame(second_encoder.get(), mic_mixer::frame_samples, -2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());
  const auto expected = decode_single_frame(second_packet);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.add_source(2));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1));
  ASSERT_TRUE(mixer.push_packet(2, second_packet.data(), second_packet.size(), 1));
  mixer.remove_source(1);

  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, expected);

  mixer.clear();
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}
