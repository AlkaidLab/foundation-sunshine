/**
 * @file src/audio_sink_plan.cpp
 * @brief See audio_sink_plan.h.
 */
#include "src/audio_sink_plan.h"

#include <algorithm>
#include <cstdlib>

namespace audio_sink_plan {
  virtual_target_e
  choose_virtual_target(const std::string &virtual_sink, const std::string &audio_sink, bool audio_sink_is_virtual) {
    if (!virtual_sink.empty()) return virtual_target_e::virtual_sink;
    if (!audio_sink.empty() && audio_sink_is_virtual) return virtual_target_e::audio_sink;
    return virtual_target_e::steam_streaming;
  }

  std::vector<int>
  layout_ladder(int stream_channels) {
    // 7.1.4 is the widest layout the host knows; every narrower one is the leading part of the wider one, so a capture in a
    // narrower layout is widened with silent channels (src/audio.cpp)
    static constexpr int known[] = { 12, 8, 6, 2 };
    std::vector<int> ladder;
    for (const int channels : known) {
      if (channels <= stream_channels) ladder.push_back(channels);
    }
    if (ladder.empty() || ladder.back() != 2) ladder.push_back(2);
    return ladder;
  }

  int
  pick_layout(int stream_channels, const std::function<bool(int)> &supported) {
    for (const int channels : layout_ladder(stream_channels)) {
      if (channels == 2 || (supported && supported(channels))) return channels;
    }
    return 2;
  }

  std::vector<std::size_t>
  format_try_order(const std::vector<int> &valid_bits, int wanted_bits) {
    std::vector<std::size_t> order(valid_bits.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (wanted_bits <= 0) return order;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return std::abs(valid_bits[a] - wanted_bits) < std::abs(valid_bits[b] - wanted_bits);
    });
    return order;
  }
}  // namespace audio_sink_plan
