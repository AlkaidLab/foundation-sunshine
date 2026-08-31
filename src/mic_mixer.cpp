/**
 * @file src/mic_mixer.cpp
 * @brief Per-session Opus decoding and host microphone mixing.
 */
#include "mic_mixer.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <opus/opus.h>

namespace mic_mixer {
  namespace {
    constexpr int channels = 1;
    constexpr std::size_t max_buffered_packets = 4;
    constexpr std::int64_t max_future_frames = 8;
    constexpr std::int64_t timestamp_discontinuity_ms = 200;
    constexpr std::size_t max_consecutive_plc_frames = jitter_buffer_frames;

    struct opus_decoder_deleter_t {
      void
      operator()(OpusDecoder *decoder) const noexcept {
        if (decoder) {
          opus_decoder_destroy(decoder);
        }
      }
    };

    using opus_decoder_t = std::unique_ptr<OpusDecoder, opus_decoder_deleter_t>;

    struct queued_packet_t {
      std::vector<std::uint8_t> payload;
    };

    struct source_t {
      opus_decoder_t decoder;
      std::optional<std::uint16_t> max_sequence;
      std::int64_t max_extended_sequence {0};
      std::optional<std::uint32_t> max_timestamp_ms;

      std::int64_t anchor_playout_slot {0};

      std::optional<std::uint16_t> expected_restart_sequence;
      boost::container::flat_map<std::int64_t, queued_packet_t> packets;
      std::vector<std::int16_t> decode_buffer = std::vector<std::int16_t>(frame_samples);
      bool playout_started {false};
      std::size_t consecutive_plc_frames {0};
    };

    std::int32_t
    sequence_distance(std::uint16_t newer, std::uint16_t older) noexcept {
      const auto distance = static_cast<std::uint16_t>(newer - older);
      return distance < 0x8000u ?
               static_cast<std::int32_t>(distance) :
               static_cast<std::int32_t>(distance) - 0x10000;
    }

    std::int64_t
    timestamp_distance(std::uint32_t newer, std::uint32_t older) noexcept {
      const auto distance = static_cast<std::uint32_t>(newer - older);
      return distance < 0x80000000u ?
               static_cast<std::int64_t>(distance) :
               static_cast<std::int64_t>(distance) - 0x100000000LL;
    }

    void
    reset_decoder(source_t &source) {
      (void) opus_decoder_ctl(source.decoder.get(), OPUS_RESET_STATE);
    }

    void
    reset_timeline(
      source_t &source,
      std::uint16_t sequence_number,
      std::optional<std::uint32_t> timestamp_ms,
      std::int64_t playout_slot
    ) {
      reset_decoder(source);
      source.max_sequence = sequence_number;
      source.max_extended_sequence = 0;
      source.max_timestamp_ms = timestamp_ms;
      source.anchor_playout_slot = playout_slot;
      source.expected_restart_sequence.reset();
      source.packets.clear();
      source.playout_started = false;
      source.consecutive_plc_frames = 0;
    }

    bool
    queue_packet(source_t &source, stats_t &stats, std::int64_t playout_slot, const std::uint8_t *data, std::size_t size) {
      auto [packet_it, inserted] = source.packets.emplace(
        playout_slot,
        queued_packet_t {std::vector<std::uint8_t> {data, data + size}}
      );
      if (!inserted) {
        ++stats.duplicate_packets;
        return false;
      }

      if (source.packets.size() <= max_buffered_packets) {
        return true;
      }

      // 实时输入优先保留最接近播放时钟的数据，最远的未来包先丢弃。
      ++stats.buffer_overflow_packets;
      auto furthest = std::prev(source.packets.end());
      const auto kept = furthest != packet_it;
      source.packets.erase(furthest);
      return kept;
    }

    bool
    decode_frame(source_t &source, const queued_packet_t *packet) {
      const auto decoded_samples = opus_decode(
        source.decoder.get(),
        packet ? packet->payload.data() : nullptr,
        packet ? static_cast<opus_int32>(packet->payload.size()) : 0,
        source.decode_buffer.data(),
        static_cast<int>(frame_samples),
        0);
      if (decoded_samples != static_cast<int>(frame_samples)) {
        return false;
      }
      return true;
    }
  }  // namespace

  struct mixer_t::impl_t {
    std::unordered_map<source_id_t, source_t> sources;
    std::int64_t next_playout_slot {0};
    std::vector<std::int64_t> mix_sums = std::vector<std::int64_t>(frame_samples);
    stats_t stats;
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
    return samples == static_cast<int>(frame_samples);
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

    source_t source;
    source.decoder = std::move(decoder);
    source.packets.reserve(max_buffered_packets + 1);
    impl_->sources.emplace(source_id, std::move(source));
    return true;
  }

  void
  mixer_t::remove_source(source_id_t source_id) {
    impl_->sources.erase(source_id);
  }

  void
  mixer_t::clear() {
    impl_->sources.clear();
    impl_->next_playout_slot = 0;
  }

  bool
  mixer_t::push_packet(
    source_id_t source_id,
    const std::uint8_t *data,
    std::size_t size,
    std::uint16_t sequence_number,
    std::optional<std::uint32_t> timestamp_ms
  ) {
    auto source_it = impl_->sources.find(source_id);
    if (source_it == impl_->sources.end() || !is_valid_opus_packet(data, size)) {
      return false;
    }

    auto &source = source_it->second;
    if (!source.max_sequence) {
      reset_timeline(
        source,
        sequence_number,
        timestamp_ms,
        impl_->next_playout_slot + static_cast<std::int64_t>(jitter_buffer_frames)
      );
      return queue_packet(source, impl_->stats, source.anchor_playout_slot, data, size);
    }

    const auto distance = sequence_distance(sequence_number, *source.max_sequence);
    if (distance == 0) {
      ++impl_->stats.duplicate_packets;
      return false;
    }

    const auto extended_sequence = source.max_extended_sequence + distance;
    const auto target_slot = source.anchor_playout_slot + extended_sequence;

    if (distance < 0 && target_slot < impl_->next_playout_slot) {
      // 参考 RFC 3550 的 source restart 检测：第一次异常回退只记录下一序列号，
      // 只有随后收到连续包才确认发送端已重启，避免单个迟到包重置解码状态。
      if (!source.expected_restart_sequence || sequence_number != *source.expected_restart_sequence) {
        source.expected_restart_sequence = static_cast<std::uint16_t>(sequence_number + 1);
        ++impl_->stats.late_packets;
        return false;
      }

      ++impl_->stats.timeline_reanchors;
      reset_timeline(
        source,
        sequence_number,
        timestamp_ms,
        impl_->next_playout_slot + static_cast<std::int64_t>(jitter_buffer_frames)
      );
      return queue_packet(source, impl_->stats, source.anchor_playout_slot, data, size);
    }

    if (distance > 0) {
      bool timestamp_discontinuous = false;
      if (timestamp_ms && source.max_timestamp_ms) {
        const auto packet_time_delta = timestamp_distance(*timestamp_ms, *source.max_timestamp_ms);
        const auto expected_time_delta = static_cast<std::int64_t>(distance) * 20;
        const auto timestamp_error = packet_time_delta - expected_time_delta;
        timestamp_discontinuous = packet_time_delta < 0 ||
                                  timestamp_error > timestamp_discontinuity_ms ||
                                  timestamp_error < -timestamp_discontinuity_ms;
      }
      const auto too_far_ahead = target_slot > impl_->next_playout_slot + max_future_frames;
      const auto inactive_timeline_expired = !source.playout_started &&
                                             target_slot < impl_->next_playout_slot;

      if (timestamp_discontinuous || too_far_ahead || inactive_timeline_expired) {
        // 客户端暂停、时钟跳变、大段丢包或非活跃时间线已过期后，
        // 不追赶旧时间线；从当前主机播放时钟重新缓冲两帧。
        ++impl_->stats.timeline_reanchors;
        reset_timeline(
          source,
          sequence_number,
          timestamp_ms,
          impl_->next_playout_slot + static_cast<std::int64_t>(jitter_buffer_frames)
        );
        return queue_packet(source, impl_->stats, source.anchor_playout_slot, data, size);
      }

      source.max_sequence = sequence_number;
      source.max_extended_sequence = extended_sequence;
      source.max_timestamp_ms = timestamp_ms;
      source.expected_restart_sequence.reset();
    }
    else {
      // 能进入尚未播放 slot 的乱序包仍属于当前时间线，应取消此前由过期包
      // 建立的重启候选，避免后续旧包误触发 source restart。
      source.expected_restart_sequence.reset();
    }

    if (target_slot < impl_->next_playout_slot) {
      ++impl_->stats.late_packets;
      return false;
    }

    return queue_packet(source, impl_->stats, target_slot, data, size);
  }

  std::optional<std::vector<std::int16_t>>
  mixer_t::mix_next_frame() {
    const auto playout_slot = impl_->next_playout_slot++;
    std::fill(impl_->mix_sums.begin(), impl_->mix_sums.end(), 0);
    std::size_t source_count = 0;
    for (auto &[source_id, source] : impl_->sources) {
      (void) source_id;

      while (!source.packets.empty() && source.packets.begin()->first < playout_slot) {
        source.packets.erase(source.packets.begin());
      }

      const queued_packet_t *packet = nullptr;
      auto packet_it = source.packets.find(playout_slot);
      if (packet_it != source.packets.end()) {
        packet = &packet_it->second;
      }
      else if (!source.playout_started) {
        continue;
      }
      else if (source.consecutive_plc_frames >= max_consecutive_plc_frames) {
        // 长时间无数据的会话不应持续参与混音分母，否则会把其他活跃客户端的音量压低。
        reset_decoder(source);
        source.playout_started = false;
        source.consecutive_plc_frames = 0;
        continue;
      }

      if (!decode_frame(source, packet)) {
        ++impl_->stats.decode_failures;
        if (packet) {
          source.packets.erase(packet_it);
        }
        reset_decoder(source);
        source.playout_started = false;
        continue;
      }

      if (packet) {
        source.packets.erase(packet_it);
        source.playout_started = true;
        source.consecutive_plc_frames = 0;
      }
      else {
        ++impl_->stats.plc_frames;
        ++source.consecutive_plc_frames;
      }

      ++source_count;
      for (std::size_t sample_index = 0; sample_index < source.decode_buffer.size(); ++sample_index) {
        impl_->mix_sums[sample_index] += source.decode_buffer[sample_index];
      }
    }

    if (source_count == 0) {
      return std::nullopt;
    }

    std::vector<std::int16_t> mixed(frame_samples, 0);
    for (std::size_t sample_index = 0; sample_index < frame_samples; ++sample_index) {
      const auto averaged = impl_->mix_sums[sample_index] / static_cast<std::int64_t>(source_count);
      mixed[sample_index] = static_cast<std::int16_t>(std::clamp<std::int64_t>(
        averaged,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
    }

    return mixed;
  }

  void
  mixer_t::skip_playout_frames(std::size_t frame_count) {
    if (frame_count == 0) {
      return;
    }

    impl_->next_playout_slot += static_cast<std::int64_t>(frame_count);
    impl_->stats.skipped_playout_frames += frame_count;
    for (auto &[source_id, source] : impl_->sources) {
      (void) source_id;
      while (!source.packets.empty() && source.packets.begin()->first < impl_->next_playout_slot) {
        source.packets.erase(source.packets.begin());
      }
      reset_decoder(source);
      source.playout_started = false;
      source.consecutive_plc_frames = 0;
    }
  }

  stats_t
  mixer_t::take_stats() {
    return std::exchange(impl_->stats, {});
  }
}  // namespace mic_mixer
