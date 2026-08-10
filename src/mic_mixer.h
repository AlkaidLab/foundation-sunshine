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

  /**
   * @brief Validate that a payload contains a decodable Opus packet shape.
   */
  bool
  is_valid_opus_packet(const std::uint8_t *data, std::size_t size);

  class mixer_t {
  public:
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
     * @brief Decode and queue one Opus packet for a source.
     */
    bool
    push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number);

    /**
     * @brief Mix one queued frame from every source into a mono PCM frame.
     * @return An empty optional when no source currently has audio queued.
     */
    std::optional<std::vector<std::int16_t>>
    mix_next_frame();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace mic_mixer
