/**
 * @file src/frame_interest.h
 * @brief Per-frame interest metadata shared by capture, analysis, and encoders.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace frame_interest {

  struct rect_t {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  struct move_rect_t {
    rect_t dest;
    int source_x = 0;
    int source_y = 0;
  };

  struct roi_rect_t {
    rect_t rect;
    int qp_delta = 0;
    int priority = 0;
  };

  enum class temporal_policy_e {
    none,
    base_only,
    base_with_discardable_enhancement,
  };

  struct map_t {
    int frame_width = 0;
    int frame_height = 0;
    std::uint64_t sequence = 0;
    bool valid = false;
    std::vector<rect_t> dirty_rects;
    std::vector<move_rect_t> move_rects;
    std::vector<roi_rect_t> roi_rects;
    temporal_policy_e temporal_policy = temporal_policy_e::none;
  };

  struct backend_caps_t {
    bool roi_qp_map = false;
    bool dirty_rects = false;
    bool move_rects = false;
    bool temporal_layers = false;
    bool long_term_reference = false;
    bool intra_refresh = false;
    bool adaptive_quantization = false;
  };

  struct backend_decision_t {
    bool roi_accepted = false;
    bool roi_fallback = false;
    bool dirty_rects_accepted = false;
    bool dirty_rects_fallback = false;
    bool move_rects_accepted = false;
    bool move_rects_fallback = false;
    bool temporal_layers_accepted = false;
    bool temporal_layers_fallback = false;
    bool uses_ltr_fallback = false;
    bool uses_intra_refresh_fallback = false;
    bool uses_aq_fallback = false;
  };

  struct qp_delta_map_t {
    int block_size = 0;
    int blocks_wide = 0;
    int blocks_high = 0;
    std::vector<std::int8_t> deltas;

    bool
    valid() const {
      return block_size > 0 && blocks_wide > 0 && blocks_high > 0 && !deltas.empty();
    }
  };

  struct qp_delta_map_policy_t {
    bool enabled = false;
    bool disable_adaptive_quantization = false;
    bool fallback_to_adaptive_quantization = false;
  };

  bool
  empty(const rect_t &rect);

  std::int64_t
  area(const rect_t &rect);

  std::int64_t
  total_dirty_area(const map_t &map);

  bool
  has_full_frame_dirty_region(const map_t &map);

  rect_t
  clamp_rect(const rect_t &rect, int frame_width, int frame_height);

  void
  add_dirty_rect(map_t &map, const rect_t &rect);

  void
  add_move_rect(map_t &map, const move_rect_t &move_rect);

  void
  add_roi_rect(map_t &map, const rect_t &rect, int qp_delta, int priority = 0);

  void
  add_cursor_roi(map_t &map, int cursor_x, int cursor_y, int radius, int qp_delta);

  void
  finalize(map_t &map);

  map_t
  scale_to_frame(const map_t &map, int frame_width, int frame_height);

  backend_decision_t
  decide_backend(const map_t &map, const backend_caps_t &caps, std::uint32_t intent_flags);

  qp_delta_map_policy_t
  decide_qp_delta_map_policy(std::uint32_t intent_flags,
    bool explicitly_enabled,
    bool adaptive_quantization_enabled);

  std::uint32_t
  encoder_qp_delta_interest_flags(std::uint32_t intent_flags, bool runtime_dynamic_interest);

  int
  nvenc_qp_delta_block_size_for_video_format(int video_format);

  qp_delta_map_t
  build_qp_delta_map(const map_t &map, int block_size, std::uint32_t intent_flags);

  const char *
  temporal_policy_name(temporal_policy_e policy);

  std::string
  summarize_map(const map_t &map);

  std::string
  summarize_decision(const backend_decision_t &decision);

}  // namespace frame_interest
