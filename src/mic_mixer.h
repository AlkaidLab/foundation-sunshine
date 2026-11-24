/**
 * @file src/mic_mixer.h
 * @brief Per-session Opus decoding and host microphone mixing.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace mic_mixer {
  using source_id_t = std::uint32_t;

  constexpr std::uint32_t sample_rate = 48000;
  constexpr std::size_t frame_samples = sample_rate / 50;
  constexpr std::size_t jitter_buffer_frames = 2;

  struct stats_t {
    std::uint64_t duplicate_packets {0};
    std::uint64_t late_packets {0};
    std::uint64_t buffer_overflow_packets {0};
    std::uint64_t timeline_reanchors {0};
    std::uint64_t plc_frames {0};
    std::uint64_t decode_failures {0};
    std::uint64_t skipped_playout_frames {0};
  };

  /**
   * @brief Validate that a payload contains a decodable Opus packet shape.
   */
  bool
  is_valid_opus_packet(const std::uint8_t *data, std::size_t size);

  class mixer_t {
  public:
    // 该类型不提供内部同步，所有成员函数都必须由麦克风接收线程串行调用。
    mixer_t();
    ~mixer_t();

    mixer_t(const mixer_t &) = delete;
    mixer_t &operator=(const mixer_t &) = delete;

    /**
     * @brief Add an independently decoded microphone source.
     */
    bool
    add_source(source_id_t source_id);

    /**
     * @brief Remove a microphone source and its queued audio.
     */
    void
    remove_source(source_id_t source_id);

    /**
     * @brief Remove every microphone source.
     */
    void
    clear();

    /**
     * @brief Queue one Opus packet for a source's playout timeline.
     */
    bool
    push_packet(
      source_id_t source_id,
      const std::uint8_t *data,
      std::size_t size,
      std::uint16_t sequence_number,
      std::optional<std::uint32_t> timestamp_ms
    );

    /**
     * @brief Mix one queued frame from every source into a mono PCM frame.
     * @return An empty optional when no source currently has audio queued.
     */
    std::optional<std::vector<std::int16_t>>
    mix_next_frame();

    /**
     * @brief Advance the playout clock without decoding missed frames.
     */
    void
    skip_playout_frames(std::size_t frame_count);

    /**
     * @brief Return and reset diagnostics accumulated by the mixer thread.
     */
    stats_t
    take_stats();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace mic_mixer
