/**
 * @file src/mic_mixer.cpp
 * @brief Per-session Opus decoding and host microphone mixing.
 */
#include "mic_mixer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <opus/opus.h>

namespace mic_mixer {
  namespace {
    constexpr int channels = 1;
    constexpr int max_opus_frame_samples = 5760;
    constexpr std::size_t max_queued_frames = 3;

    struct opus_decoder_deleter_t {
      void
      operator()(OpusDecoder *decoder) const noexcept {
        if (decoder) {
          opus_decoder_destroy(decoder);
        }
      }
    };

    using opus_decoder_t = std::unique_ptr<OpusDecoder, opus_decoder_deleter_t>;

    struct source_t {
      opus_decoder_t decoder;
      std::optional<std::uint16_t> last_sequence;
      std::deque<std::vector<std::int16_t>> frames;
    };

    void
    queue_frame(source_t &source, std::vector<std::int16_t> frame) {
      if (source.frames.size() >= max_queued_frames) {
        source.frames.pop_front();
      }
      source.frames.emplace_back(std::move(frame));
    }

    bool
    decode_frame(source_t &source, const std::uint8_t *data, std::size_t size) {
      const auto frame_size = opus_decoder_get_nb_samples(
        source.decoder.get(),
        data,
        static_cast<opus_int32>(size));
      if (frame_size <= 0 || frame_size > max_opus_frame_samples) {
        return false;
      }

      std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_size));
      const auto decoded_samples = opus_decode(
        source.decoder.get(),
        data,
        static_cast<opus_int32>(size),
        pcm.data(),
        frame_size,
        0);
      if (decoded_samples <= 0) {
        return false;
      }

      pcm.resize(static_cast<std::size_t>(decoded_samples));
      queue_frame(source, std::move(pcm));
      return true;
    }
  }  // namespace

  struct mixer_t::impl_t {
    std::unordered_map<source_id_t, source_t> sources;
  };

  mixer_t::mixer_t():
      impl_ {std::make_unique<impl_t>()} {}

  mixer_t::~mixer_t() = default;

  bool
  is_valid_opus_packet(const std::uint8_t *data, std::size_t size) {
    if (!data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<opus_int32>::max())) {
      return false;
    }
    const auto samples = opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), sample_rate);
    return samples > 0 && samples <= max_opus_frame_samples;
  }

  bool
  mixer_t::add_source(source_id_t source_id) {
    if (impl_->sources.contains(source_id)) {
      return true;
    }

    int error = OPUS_OK;
    opus_decoder_t decoder {opus_decoder_create(sample_rate, channels, &error)};
    if (!decoder || error != OPUS_OK) {
      return false;
    }

    impl_->sources.emplace(source_id, source_t {std::move(decoder), std::nullopt, {}});
    return true;
  }

  void
  mixer_t::remove_source(source_id_t source_id) {
    impl_->sources.erase(source_id);
  }

  void
  mixer_t::clear() {
    impl_->sources.clear();
  }

  bool
  mixer_t::push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number) {
    auto source_it = impl_->sources.find(source_id);
    if (source_it == impl_->sources.end() || !data || size == 0) {
      return false;
    }

    auto &source = source_it->second;
    if (source.last_sequence) {
      const auto distance = static_cast<std::uint16_t>(sequence_number - *source.last_sequence);
      if (distance == 0 || distance >= 0x8000) {
        return false;
      }
    }

    // 混音由固定的 20 ms 时钟驱动。迟到的 FEC 帧已经错过原时间片，
    // 再插入队列会让该音源持续落后一帧，因此这里只解码当前数据包。
    if (!decode_frame(source, data, size)) {
      return false;
    }

    source.last_sequence = sequence_number;
    return true;
  }

  std::optional<std::vector<std::int16_t>>
  mixer_t::mix_next_frame() {
    std::size_t output_samples = 0;
    std::size_t source_count = 0;
    for (const auto &[source_id, source] : impl_->sources) {
      (void) source_id;
      if (!source.frames.empty()) {
        output_samples = std::max(output_samples, source.frames.front().size());
        ++source_count;
      }
    }

    if (source_count == 0 || output_samples == 0) {
      return std::nullopt;
    }

    std::vector<std::int64_t> sums(output_samples, 0);
    std::vector<std::size_t> contributors(output_samples, 0);
    for (auto &[source_id, source] : impl_->sources) {
      (void) source_id;
      if (source.frames.empty()) {
        continue;
      }

      auto frame = std::move(source.frames.front());
      source.frames.pop_front();
      for (std::size_t sample_index = 0; sample_index < frame.size(); ++sample_index) {
        sums[sample_index] += frame[sample_index];
        ++contributors[sample_index];
      }
    }

    std::vector<std::int16_t> mixed(output_samples, 0);
    for (std::size_t sample_index = 0; sample_index < output_samples; ++sample_index) {
      if (contributors[sample_index] == 0) {
        continue;
      }

      const auto averaged = sums[sample_index] / static_cast<std::int64_t>(contributors[sample_index]);
      mixed[sample_index] = static_cast<std::int16_t>(std::clamp<std::int64_t>(
        averaged,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
    }

    return mixed;
  }
}  // namespace mic_mixer
