/**
 * @file tests/unit/test_mic_mixer.cpp
 * @brief Test src/mic_mixer.*.
 */
#include <src/mic_mixer.h>

#include "../tests_common.h"

#include <cstdint>
#include <memory>
#include <utility>
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
    EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 1, 20));
    for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
      EXPECT_FALSE(mixer.mix_next_frame().has_value());
    }
    auto decoded = mixer.mix_next_frame();
    EXPECT_TRUE(decoded.has_value());
    return decoded.value_or(std::vector<std::int16_t> {});
  }

  std::vector<std::vector<std::int16_t>>
  decode_frames_in_sequence(const std::vector<std::vector<std::uint8_t>> &packets) {
    mic_mixer::mixer_t mixer;
    EXPECT_TRUE(mixer.add_source(1));

    std::vector<std::vector<std::int16_t>> decoded_frames;
    decoded_frames.reserve(packets.size());
    for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
      const auto &packet = packets[packet_index];
      EXPECT_TRUE(mixer.push_packet(
        1,
        packet.data(),
        packet.size(),
        static_cast<std::uint16_t>(packet_index + 1),
        static_cast<std::uint32_t>((packet_index + 1) * 20)));
    }

    for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
      EXPECT_FALSE(mixer.mix_next_frame().has_value());
    }
    for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
      auto decoded = mixer.mix_next_frame();
      EXPECT_TRUE(decoded.has_value());
      if (!decoded) {
        return {};
      }
      decoded_frames.emplace_back(std::move(*decoded));
    }
    return decoded_frames;
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
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(2, second_packet.data(), second_packet.size(), 1, 20));

  for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
    EXPECT_FALSE(mixer.mix_next_frame().has_value());
  }

  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  ASSERT_EQ(mixed->size(), mic_mixer::frame_samples);
  for (std::size_t sample = 0; sample < mixed->size(); ++sample) {
    const auto expected = (static_cast<std::int64_t>(first_decoded[sample]) +
                           static_cast<std::int64_t>(second_decoded[sample])) /
                          2;
    EXPECT_EQ((*mixed)[sample], expected);
  }
}

TEST(MicMixerTest, KeepsOnlyNearestFuturePackets) {
  auto encoder = make_encoder();
  std::vector<std::vector<std::uint8_t>> packets;
  for (const auto sample_value : {1000, 2000, 3000, 4000, 5000}) {
    packets.emplace_back(encode_frame(encoder.get(), mic_mixer::frame_samples, sample_value));
    ASSERT_FALSE(packets.back().empty());
  }

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
    const auto &packet = packets[packet_index];
    const auto queued = mixer.push_packet(
      1,
      packet.data(),
      packet.size(),
      static_cast<std::uint16_t>(packet_index + 1),
      static_cast<std::uint32_t>((packet_index + 1) * 20));
    EXPECT_EQ(queued, packet_index < 4);
  }
}

TEST(MicMixerTest, RejectsDuplicatesAndAcceptsSequenceWraparound) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 0xFFFF, 20));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 0, 40));
  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 0, 40));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, AcceptsMillisecondTimestampWraparound) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto second_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  EXPECT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 0xFFFFFFF0u));
  EXPECT_TRUE(mixer.push_packet(1, second_packet.data(), second_packet.size(), 2, 4));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, MissingTimestampStillUsesTheSequenceTimeline) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto second_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());
  const auto expected = decode_frames_in_sequence({first_packet, second_packet});
  ASSERT_EQ(expected.size(), 2);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  EXPECT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, std::nullopt));
  EXPECT_TRUE(mixer.push_packet(1, second_packet.data(), second_packet.size(), 2, std::nullopt));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto first = mixer.mix_next_frame();
  const auto second = mixer.mix_next_frame();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, expected[0]);
  EXPECT_EQ(*second, expected[1]);
}

TEST(MicMixerTest, SingleLatePacketDoesNotResetSequenceState) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 100, 2000));
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  ASSERT_TRUE(mixer.mix_next_frame().has_value());

  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 90, 1800));
  EXPECT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 101, 2020));
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
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
  ASSERT_TRUE(mixer.push_packet(1, old_packet.data(), old_packet.size(), 100, 2000));
  EXPECT_FALSE(mixer.push_packet(1, restart_packet.data(), restart_packet.size(), 10, 20));
  EXPECT_TRUE(mixer.push_packet(1, restart_packet.data(), restart_packet.size(), 11, 40));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, decode_single_frame(restart_packet));
}

TEST(MicMixerTest, AcceptedReorderedPacketCancelsRestartCandidate) {
  auto encoder = make_encoder();
  const auto ninth_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 900);
  const auto tenth_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto stale_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 200);
  ASSERT_FALSE(ninth_packet.empty());
  ASSERT_FALSE(tenth_packet.empty());
  ASSERT_FALSE(stale_packet.empty());
  const auto expected = decode_frames_in_sequence({ninth_packet, tenth_packet});
  ASSERT_EQ(expected.size(), 2);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, tenth_packet.data(), tenth_packet.size(), 10, 200));
  EXPECT_FALSE(mixer.push_packet(1, stale_packet.data(), stale_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(1, ninth_packet.data(), ninth_packet.size(), 9, 180));
  EXPECT_FALSE(mixer.push_packet(1, stale_packet.data(), stale_packet.size(), 2, 40));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto ninth = mixer.mix_next_frame();
  const auto tenth = mixer.mix_next_frame();
  ASSERT_TRUE(ninth.has_value());
  ASSERT_TRUE(tenth.has_value());
  EXPECT_EQ(*ninth, expected[0]);
  EXPECT_EQ(*tenth, expected[1]);
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
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(2, second_packet.data(), second_packet.size(), 1, 20));
  mixer.remove_source(1);

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, expected);

  mixer.clear();
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, ReordersPacketsBeforePlayout) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto second_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());
  const auto expected = decode_frames_in_sequence({first_packet, second_packet});
  ASSERT_EQ(expected.size(), 2);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, second_packet.data(), second_packet.size(), 2, 40));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto first = mixer.mix_next_frame();
  const auto second = mixer.mix_next_frame();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, expected[0]);
  EXPECT_EQ(*second, expected[1]);
}

TEST(MicMixerTest, ConcealsOneMissingFrame) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto third_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 3000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(third_packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(1, third_packet.data(), third_packet.size(), 3, 60));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());  // Opus PLC for sequence 2
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, CountsAnOrderedPacketThatArrivesAfterPlc) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto late_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(late_packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());  // PLC for sequence 2

  EXPECT_FALSE(mixer.push_packet(1, late_packet.data(), late_packet.size(), 2, 40));
  const auto stats = mixer.take_stats();
  EXPECT_EQ(stats.plc_frames, 1);
  EXPECT_EQ(stats.late_packets, 1);
}

TEST(MicMixerTest, StopsConcealmentForAnInactiveSource) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 1, 20));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
    EXPECT_TRUE(mixer.mix_next_frame().has_value());
  }
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_EQ(mixer.take_stats().plc_frames, mic_mixer::jitter_buffer_frames);
}

TEST(MicMixerTest, ReportsAndResetsQueueDiagnostics) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  for (std::uint16_t sequence = 1; sequence <= 4; ++sequence) {
    ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), sequence, sequence * 20));
  }
  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 5, 100));
  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 5, 100));
  mixer.skip_playout_frames(3);

  const auto stats = mixer.take_stats();
  EXPECT_EQ(stats.buffer_overflow_packets, 1);
  EXPECT_EQ(stats.duplicate_packets, 1);
  EXPECT_EQ(stats.skipped_playout_frames, 3);

  const auto reset_stats = mixer.take_stats();
  EXPECT_EQ(reset_stats.buffer_overflow_packets, 0);
  EXPECT_EQ(reset_stats.duplicate_packets, 0);
  EXPECT_EQ(reset_stats.skipped_playout_frames, 0);
}

TEST(MicMixerTest, ReportsLatePacketsAndConfirmedTimelineReanchors) {
  auto encoder = make_encoder();
  const auto packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  ASSERT_FALSE(packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 10, 200));
  EXPECT_FALSE(mixer.push_packet(1, packet.data(), packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(1, packet.data(), packet.size(), 2, 40));

  const auto stats = mixer.take_stats();
  EXPECT_EQ(stats.late_packets, 1);
  EXPECT_EQ(stats.timeline_reanchors, 1);
}

TEST(MicMixerTest, ReanchorsAnInactiveSourceThatResumesWithContinuousTimestamps) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto resumed_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(resumed_packet.empty());

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
  for (std::size_t frame = 0; frame < mic_mixer::jitter_buffer_frames; ++frame) {
    EXPECT_TRUE(mixer.mix_next_frame().has_value());
  }
  EXPECT_FALSE(mixer.mix_next_frame().has_value());

  ASSERT_TRUE(mixer.push_packet(1, resumed_packet.data(), resumed_packet.size(), 2, 40));
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_TRUE(mixer.mix_next_frame().has_value());
}

TEST(MicMixerTest, TimestampJumpReanchorsTheSource) {
  auto encoder = make_encoder();
  const auto old_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto reanchored_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 3000);
  ASSERT_FALSE(old_packet.empty());
  ASSERT_FALSE(reanchored_packet.empty());
  const auto expected = decode_single_frame(reanchored_packet);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, old_packet.data(), old_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(1, reanchored_packet.data(), reanchored_packet.size(), 2, 5000));

  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, expected);
}

TEST(MicMixerTest, SkippingHostFramesDropsExpiredPackets) {
  auto encoder = make_encoder();
  const auto expired_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto current_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  ASSERT_FALSE(expired_packet.empty());
  ASSERT_FALSE(current_packet.empty());
  const auto expected = decode_single_frame(current_packet);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, expired_packet.data(), expired_packet.size(), 1, 20));
  mixer.skip_playout_frames(3);

  ASSERT_TRUE(mixer.push_packet(1, current_packet.data(), current_packet.size(), 2, 40));
  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, expected);
}

TEST(MicMixerTest, SkippingHostFramesKeepsCurrentAndFuturePackets) {
  auto encoder = make_encoder();
  const auto first_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 1000);
  const auto second_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 2000);
  const auto third_packet = encode_frame(encoder.get(), mic_mixer::frame_samples, 3000);
  ASSERT_FALSE(first_packet.empty());
  ASSERT_FALSE(second_packet.empty());
  ASSERT_FALSE(third_packet.empty());
  const auto expected = decode_frames_in_sequence({second_packet, third_packet});
  ASSERT_EQ(expected.size(), 2);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, first_packet.data(), first_packet.size(), 1, 20));
  ASSERT_TRUE(mixer.push_packet(1, second_packet.data(), second_packet.size(), 2, 40));
  ASSERT_TRUE(mixer.push_packet(1, third_packet.data(), third_packet.size(), 3, 60));

  mixer.skip_playout_frames(3);

  const auto second = mixer.mix_next_frame();
  const auto third = mixer.mix_next_frame();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(*second, expected[0]);
  EXPECT_EQ(*third, expected[1]);
}

TEST(MicMixerTest, ClearDropsQueuedAudioAndRebuffersTheNewSource) {
  auto old_encoder = make_encoder();
  auto new_encoder = make_encoder();
  const auto old_packet = encode_frame(old_encoder.get(), mic_mixer::frame_samples, 1000);
  const auto new_packet = encode_frame(new_encoder.get(), mic_mixer::frame_samples, 3000);
  ASSERT_FALSE(old_packet.empty());
  ASSERT_FALSE(new_packet.empty());
  const auto expected = decode_single_frame(new_packet);

  mic_mixer::mixer_t mixer;
  ASSERT_TRUE(mixer.add_source(1));
  ASSERT_TRUE(mixer.push_packet(1, old_packet.data(), old_packet.size(), 1, 20));
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  mixer.clear();

  ASSERT_TRUE(mixer.add_source(2));
  ASSERT_TRUE(mixer.push_packet(2, new_packet.data(), new_packet.size(), 1, 20));
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  EXPECT_FALSE(mixer.mix_next_frame().has_value());
  const auto mixed = mixer.mix_next_frame();
  ASSERT_TRUE(mixed.has_value());
  EXPECT_EQ(*mixed, expected);
}
