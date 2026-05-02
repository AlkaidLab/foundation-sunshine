/**
 * @file src/frame_interest.cpp
 * @brief Per-frame interest metadata helpers.
 */
#include "frame_interest.h"

#include "stream_quality.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

namespace frame_interest {
  namespace {
    bool
    intersects_or_touches(const rect_t &a, const rect_t &b) {
      return a.x <= b.x + b.width &&
             b.x <= a.x + a.width &&
             a.y <= b.y + b.height &&
             b.y <= a.y + a.height;
    }

    rect_t
    union_rect(const rect_t &a, const rect_t &b) {
      const auto left = std::min(a.x, b.x);
      const auto top = std::min(a.y, b.y);
      const auto right = std::max(a.x + a.width, b.x + b.width);
      const auto bottom = std::max(a.y + a.height, b.y + b.height);
      return { left, top, right - left, bottom - top };
    }

    void
    merge_rects(std::vector<rect_t> &rects) {
      bool changed = true;
      while (changed) {
        changed = false;
        for (auto i = rects.begin(); i != rects.end() && !changed; ++i) {
          for (auto j = std::next(i); j != rects.end(); ++j) {
            if (intersects_or_touches(*i, *j)) {
              *i = union_rect(*i, *j);
              rects.erase(j);
              changed = true;
              break;
            }
          }
        }
      }
    }

    int
    scale_coord(int value, double scale) {
      return static_cast<int>(std::lround(static_cast<double>(value) * scale));
    }

    rect_t
    scale_rect(const rect_t &rect, double scale_x, double scale_y) {
      const auto left = scale_coord(rect.x, scale_x);
      const auto top = scale_coord(rect.y, scale_y);
      const auto right = scale_coord(rect.x + rect.width, scale_x);
      const auto bottom = scale_coord(rect.y + rect.height, scale_y);
      return { left, top, right - left, bottom - top };
    }
  }  // namespace

  bool
  empty(const rect_t &rect) {
    return rect.width <= 0 || rect.height <= 0;
  }

  std::int64_t
  area(const rect_t &rect) {
    if (empty(rect)) {
      return 0;
    }

    return static_cast<std::int64_t>(rect.width) * static_cast<std::int64_t>(rect.height);
  }

  std::int64_t
  total_dirty_area(const map_t &map) {
    std::int64_t total = 0;
    for (const auto &rect : map.dirty_rects) {
      total += area(rect);
    }
    return total;
  }

  bool
  has_full_frame_dirty_region(const map_t &map) {
    if (map.frame_width <= 0 || map.frame_height <= 0 || map.dirty_rects.empty()) {
      return false;
    }

    const auto frame_area = static_cast<std::int64_t>(map.frame_width) *
                            static_cast<std::int64_t>(map.frame_height);
    if (frame_area <= 0) {
      return false;
    }

    return static_cast<double>(total_dirty_area(map)) /
             static_cast<double>(frame_area) >= 0.85;
  }

  rect_t
  clamp_rect(const rect_t &rect, int frame_width, int frame_height) {
    if (frame_width <= 0 || frame_height <= 0 || empty(rect)) {
      return {};
    }

    const auto left = std::clamp(rect.x, 0, frame_width);
    const auto top = std::clamp(rect.y, 0, frame_height);
    const auto right = std::clamp(rect.x + rect.width, 0, frame_width);
    const auto bottom = std::clamp(rect.y + rect.height, 0, frame_height);
    if (right <= left || bottom <= top) {
      return {};
    }

    return { left, top, right - left, bottom - top };
  }

  void
  add_dirty_rect(map_t &map, const rect_t &rect) {
    map.dirty_rects.push_back(rect);
  }

  void
  add_move_rect(map_t &map, const move_rect_t &move_rect) {
    map.move_rects.push_back(move_rect);
  }

  void
  add_roi_rect(map_t &map, const rect_t &rect, int qp_delta, int priority) {
    map.roi_rects.push_back({
      .rect = rect,
      .qp_delta = std::clamp(qp_delta, -15, 15),
      .priority = priority,
    });
  }

  void
  add_cursor_roi(map_t &map, int cursor_x, int cursor_y, int radius, int qp_delta) {
    radius = std::max(1, radius);
    add_roi_rect(map,
                 { cursor_x - radius, cursor_y - radius, radius * 2, radius * 2 },
                 qp_delta,
                 100);
  }

  void
  finalize(map_t &map) {
    auto clamp_dirty = [&](rect_t rect) {
      return clamp_rect(rect, map.frame_width, map.frame_height);
    };

    std::vector<rect_t> dirty;
    dirty.reserve(map.dirty_rects.size());
    for (const auto &rect : map.dirty_rects) {
      auto clamped = clamp_dirty(rect);
      if (!empty(clamped)) {
        dirty.push_back(clamped);
      }
    }
    merge_rects(dirty);
    map.dirty_rects = std::move(dirty);

    std::vector<move_rect_t> moves;
    moves.reserve(map.move_rects.size());
    for (auto move_rect : map.move_rects) {
      move_rect.dest = clamp_rect(move_rect.dest, map.frame_width, map.frame_height);
      move_rect.source_x = std::clamp(move_rect.source_x, 0, std::max(0, map.frame_width));
      move_rect.source_y = std::clamp(move_rect.source_y, 0, std::max(0, map.frame_height));
      if (!empty(move_rect.dest)) {
        moves.push_back(move_rect);
      }
    }
    map.move_rects = std::move(moves);

    std::vector<roi_rect_t> rois;
    rois.reserve(map.roi_rects.size());
    for (auto roi : map.roi_rects) {
      roi.rect = clamp_rect(roi.rect, map.frame_width, map.frame_height);
      roi.qp_delta = std::clamp(roi.qp_delta, -15, 15);
      if (!empty(roi.rect)) {
        rois.push_back(roi);
      }
    }
    map.roi_rects = std::move(rois);
    map.valid = map.frame_width > 0 &&
                map.frame_height > 0 &&
                (!map.dirty_rects.empty() ||
                 !map.move_rects.empty() ||
                 !map.roi_rects.empty() ||
                map.temporal_policy != temporal_policy_e::none);
  }

  map_t
  scale_to_frame(const map_t &map, int frame_width, int frame_height) {
    if (frame_width <= 0 ||
        frame_height <= 0 ||
        map.frame_width <= 0 ||
        map.frame_height <= 0 ||
        (map.frame_width == frame_width && map.frame_height == frame_height)) {
      auto result = map;
      result.frame_width = frame_width > 0 ? frame_width : map.frame_width;
      result.frame_height = frame_height > 0 ? frame_height : map.frame_height;
      return result;
    }

    const auto scale_x = static_cast<double>(frame_width) / static_cast<double>(map.frame_width);
    const auto scale_y = static_cast<double>(frame_height) / static_cast<double>(map.frame_height);

    map_t result;
    result.frame_width = frame_width;
    result.frame_height = frame_height;
    result.sequence = map.sequence;
    result.temporal_policy = map.temporal_policy;

    result.dirty_rects.reserve(map.dirty_rects.size());
    for (const auto &rect : map.dirty_rects) {
      result.dirty_rects.push_back(scale_rect(rect, scale_x, scale_y));
    }

    result.move_rects.reserve(map.move_rects.size());
    for (const auto &move : map.move_rects) {
      result.move_rects.push_back({
        .dest = scale_rect(move.dest, scale_x, scale_y),
        .source_x = scale_coord(move.source_x, scale_x),
        .source_y = scale_coord(move.source_y, scale_y),
      });
    }

    result.roi_rects.reserve(map.roi_rects.size());
    for (const auto &roi : map.roi_rects) {
      result.roi_rects.push_back({
        .rect = scale_rect(roi.rect, scale_x, scale_y),
        .qp_delta = roi.qp_delta,
        .priority = roi.priority,
      });
    }

    finalize(result);
    return result;
  }

  backend_decision_t
  decide_backend(const map_t &map, const backend_caps_t &caps, std::uint32_t intent_flags) {
    backend_decision_t decision {};
    const bool wants_roi = (intent_flags & stream_quality::clarity_intent_roi) != 0 && !map.roi_rects.empty();
    const bool full_frame_dirty = has_full_frame_dirty_region(map);
    const bool wants_dirty = (intent_flags & stream_quality::clarity_intent_dirty_region) != 0 &&
                             !map.dirty_rects.empty() &&
                             !full_frame_dirty;
    const bool wants_move = !map.move_rects.empty();
    const bool wants_temporal = (intent_flags & stream_quality::clarity_intent_temporal_layers) != 0 &&
                                map.temporal_policy != temporal_policy_e::none;

    decision.roi_accepted = wants_roi && caps.roi_qp_map;
    decision.roi_fallback = wants_roi && !decision.roi_accepted;
    decision.dirty_rects_accepted = wants_dirty && caps.dirty_rects;
    decision.dirty_rects_fallback = wants_dirty && !decision.dirty_rects_accepted;
    decision.move_rects_accepted = wants_move && caps.move_rects;
    decision.move_rects_fallback = wants_move && !decision.move_rects_accepted;
    decision.temporal_layers_accepted = wants_temporal && caps.temporal_layers;
    decision.temporal_layers_fallback = wants_temporal && !decision.temporal_layers_accepted;

    const bool needs_fallback = decision.roi_fallback ||
                                decision.dirty_rects_fallback ||
                                decision.move_rects_fallback ||
                                decision.temporal_layers_fallback;
    decision.uses_ltr_fallback = needs_fallback && caps.long_term_reference;
    decision.uses_intra_refresh_fallback = needs_fallback && caps.intra_refresh;
    decision.uses_aq_fallback = needs_fallback && caps.adaptive_quantization;

    std::ostringstream reason;
    auto append_reason = [&](const char *value) {
      if (reason.tellp() > 0) {
        reason << ",";
      }
      reason << value;
    };
    if (decision.roi_fallback) {
      append_reason("roi-qp-map-not-encoder-applied");
    }
    if (decision.dirty_rects_fallback) {
      append_reason(full_frame_dirty ? "dirty-full-frame-motion-not-savings" : "dirty-qp-map-not-encoder-applied");
    }
    if (decision.move_rects_fallback) {
      append_reason("move-rect-backend-unavailable");
    }
    if (decision.temporal_layers_fallback) {
      append_reason("temporal-svc-disabled-pending-validation");
    }
    decision.fallback_reason = reason.str();
    return decision;
  }

  qp_delta_map_policy_t
  decide_qp_delta_map_policy(std::uint32_t intent_flags,
    bool explicitly_enabled,
    bool adaptive_quantization_enabled) {
    qp_delta_map_policy_t policy {};
    const bool wants_qp_map =
      (intent_flags & (stream_quality::clarity_intent_roi |
                       stream_quality::clarity_intent_dirty_region)) != 0;

    if (!wants_qp_map) {
      return policy;
    }

    if (!explicitly_enabled) {
      policy.fallback_to_adaptive_quantization = adaptive_quantization_enabled;
      return policy;
    }

    policy.enabled = true;
    policy.disable_adaptive_quantization = adaptive_quantization_enabled;
    return policy;
  }

  std::uint32_t
  encoder_qp_delta_interest_flags(std::uint32_t intent_flags, bool runtime_dynamic_interest) {
    if (runtime_dynamic_interest) {
      intent_flags |= stream_quality::clarity_intent_roi |
                      stream_quality::clarity_intent_dirty_region |
                      stream_quality::clarity_intent_temporal_layers |
                      stream_quality::clarity_intent_discardable_enhancement |
                      stream_quality::clarity_intent_long_term_reference |
                      stream_quality::clarity_intent_intra_refresh;
    }
    return intent_flags;
  }

  int
  nvenc_qp_delta_block_size_for_video_format(int video_format) {
    switch (video_format) {
      case 0:  // H.264 macroblock
        return 16;
      case 1:  // HEVC CTB; NVENC supports maxCUSize 32x32
        return 32;
      case 2:  // AV1 superblock
        return 64;
      default:
        return 16;
    }
  }

  qp_delta_map_t
  build_qp_delta_map(const map_t &map, int block_size, std::uint32_t intent_flags) {
    qp_delta_map_t qp_map;
    if (!map.valid || block_size <= 0 || map.frame_width <= 0 || map.frame_height <= 0) {
      return qp_map;
    }

    const bool wants_roi = (intent_flags & stream_quality::clarity_intent_roi) != 0;
    const bool wants_dirty = (intent_flags & stream_quality::clarity_intent_dirty_region) != 0 &&
                             !has_full_frame_dirty_region(map);
    if ((!wants_roi || map.roi_rects.empty()) && (!wants_dirty || map.dirty_rects.empty())) {
      return qp_map;
    }

    qp_map.block_size = block_size;
    qp_map.blocks_wide = static_cast<int>(std::ceil(static_cast<double>(map.frame_width) /
                                                    static_cast<double>(block_size)));
    qp_map.blocks_high = static_cast<int>(std::ceil(static_cast<double>(map.frame_height) /
                                                    static_cast<double>(block_size)));
    qp_map.deltas.assign(static_cast<std::size_t>(qp_map.blocks_wide * qp_map.blocks_high), 0);

    auto apply_rect = [&](const rect_t &rect, std::int8_t delta) {
      auto clamped = clamp_rect(rect, map.frame_width, map.frame_height);
      if (empty(clamped)) {
        return;
      }

      const auto left = std::clamp(clamped.x / block_size, 0, qp_map.blocks_wide - 1);
      const auto top = std::clamp(clamped.y / block_size, 0, qp_map.blocks_high - 1);
      const auto right = std::clamp((clamped.x + clamped.width - 1) / block_size, 0, qp_map.blocks_wide - 1);
      const auto bottom = std::clamp((clamped.y + clamped.height - 1) / block_size, 0, qp_map.blocks_high - 1);
      for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
          auto &cell = qp_map.deltas[static_cast<std::size_t>(y * qp_map.blocks_wide + x)];
          cell = std::min(cell, delta);
        }
      }
    };

    if (wants_dirty) {
      for (const auto &rect : map.dirty_rects) {
        apply_rect(rect, -1);
      }
    }

    if (wants_roi) {
      for (const auto &roi : map.roi_rects) {
        apply_rect(roi.rect, static_cast<std::int8_t>(std::clamp(roi.qp_delta, -15, 15)));
      }
    }

    return qp_map;
  }

  const char *
  temporal_policy_name(temporal_policy_e policy) {
    switch (policy) {
      case temporal_policy_e::none:
        return "none";
      case temporal_policy_e::base_only:
        return "base-only";
      case temporal_policy_e::base_with_discardable_enhancement:
        return "base+discardable-enhancement";
    }

    return "unknown";
  }

  std::string
  summarize_map(const map_t &map) {
    std::ostringstream ss;
    ss << "valid=" << (map.valid ? 1 : 0)
       << " frame=" << map.frame_width << "x" << map.frame_height
       << " dirtyRects=" << map.dirty_rects.size()
       << " dirtyArea=" << total_dirty_area(map)
       << " fullFrameDirty=" << (has_full_frame_dirty_region(map) ? 1 : 0)
       << " moveRects=" << map.move_rects.size()
       << " roiRects=" << map.roi_rects.size()
       << " temporal=" << temporal_policy_name(map.temporal_policy);
    return ss.str();
  }

  std::string
  summarize_decision(const backend_decision_t &decision) {
    std::ostringstream ss;
    ss << "roi(accepted/fallback)=" << (decision.roi_accepted ? 1 : 0) << "/" << (decision.roi_fallback ? 1 : 0)
       << " dirty(accepted/fallback)=" << (decision.dirty_rects_accepted ? 1 : 0) << "/" << (decision.dirty_rects_fallback ? 1 : 0)
       << " move(accepted/fallback)=" << (decision.move_rects_accepted ? 1 : 0) << "/" << (decision.move_rects_fallback ? 1 : 0)
       << " temporal(accepted/fallback)=" << (decision.temporal_layers_accepted ? 1 : 0) << "/" << (decision.temporal_layers_fallback ? 1 : 0)
       << " fallback(ltr/intra/aq)=" << (decision.uses_ltr_fallback ? 1 : 0) << "/"
       << (decision.uses_intra_refresh_fallback ? 1 : 0) << "/"
       << (decision.uses_aq_fallback ? 1 : 0);
    if (!decision.fallback_reason.empty()) {
      ss << " fallbackReason=" << decision.fallback_reason;
    }
    return ss.str();
  }

  std::string
  summarize_backend_caps(const backend_caps_t &caps) {
    std::ostringstream ss;
    ss << "caps(roiQpMap/dirty/move/temporal/ltr/intra/aq)="
       << (caps.roi_qp_map ? 1 : 0) << "/"
       << (caps.dirty_rects ? 1 : 0) << "/"
       << (caps.move_rects ? 1 : 0) << "/"
       << (caps.temporal_layers ? 1 : 0) << "/"
       << (caps.long_term_reference ? 1 : 0) << "/"
       << (caps.intra_refresh ? 1 : 0) << "/"
       << (caps.adaptive_quantization ? 1 : 0);
    return ss.str();
  }
}  // namespace frame_interest
