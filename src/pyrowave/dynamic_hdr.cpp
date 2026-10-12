#include "dynamic_hdr.h"

#include "src/hdr/dynamic_hdr_selection.h"
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <span>

namespace pyrowave {
  namespace {

    bool
    usable_scene_sample(const platf::hdr_frame_luminance_stats_t &stats,
      std::uint16_t type, std::uint16_t target_peak_nits) {
      const auto valid_nits = [](float value) { return std::isfinite(value) && value >= 0.0f; };
      const auto valid_pq = [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; };
      if (!stats.valid || !valid_nits(stats.min_maxrgb) || !valid_nits(stats.max_maxrgb) ||
          !valid_nits(stats.avg_maxrgb) || !valid_nits(stats.percentile_99) ||
          !valid_pq(stats.avg_maxrgb_pq) || !valid_pq(stats.percentile_10_pq) ||
          !valid_pq(stats.percentile_90_pq) ||
          !std::all_of(std::begin(stats.distribution_maxrgb), std::end(stats.distribution_maxrgb), valid_nits)) {
        return false;
      }
      if (type == LI_PYROWAVE_METADATA_DOLBY_VISION_RPU) {
        return video::dolby_vision::frame_metadata_from_stats(stats).has_value();
      }
      return type != LI_PYROWAVE_METADATA_HDR_VIVID ||
             video::hdr_metadata::vivid_from_stats(stats, target_peak_nits).valid;
    }

  }  // namespace

  bool
  hdr_metadata_producer_t::configure(int format, const SS_HDR_METADATA &source, std::uint16_t target_peak_nits) {
    format_ = 0;
    frame_ = {};
    builder_.reset();
    scenes_.reset();
    rpu_.reset();
    level1_filter_.reset();
    last_scene_stats_ = {};
    if (format == 0) {
      return true;
    }
    frame_.type = LiPyrowaveDynamicHdrMetadataType(format);
    if (frame_.type == 0) {
      return false;
    }
    target_peak_nits_ = std::clamp<std::uint16_t>(target_peak_nits, 1, 10000);
    if (format == static_cast<int>(hdr::dynamic_hdr_format_e::vivid_hlg) ||
        format == static_cast<int>(hdr::dynamic_hdr_format_e::dolby_vision_profile_84)) {
      // Match display_vram's HLG inverse OOTF reference, not the receiving
      // display target and not the content's 99th-percentile statistic.
      frame_.hlg_nominal_peak_nits = source.maxDisplayLuminance != 0 ? source.maxDisplayLuminance : 1000;
    }
    builder_.configure({
      .hdr10plus = format == static_cast<int>(hdr::dynamic_hdr_format_e::hdr10_plus),
      .vivid = format == static_cast<int>(hdr::dynamic_hdr_format_e::vivid_pq) ||
               format == static_cast<int>(hdr::dynamic_hdr_format_e::vivid_hlg),
    });
    if (frame_.type == LI_PYROWAVE_METADATA_DOLBY_VISION_RPU) {
      if (source.maxDisplayLuminance == 0) {
        return false;
      }
      video::dolby_vision::session_config_t config {
        .source_mastering_peak_nits = std::clamp<std::uint16_t>(source.maxDisplayLuminance, 1, 10000),
        .mastering_min_nits_x10000 = std::clamp<std::uint16_t>(source.minDisplayLuminance, 1, 10000),
        .max_cll_nits = std::min<std::uint16_t>(source.maxContentLightLevel, 10000),
        .max_fall_nits = std::min<std::uint16_t>(source.maxFrameAverageLightLevel, 10000),
      };
      if (!rpu_.configure(config)) {
        return false;
      }
    }
    format_ = format;
    return true;
  }

  const hdr_frame_metadata_t *
  hdr_metadata_producer_t::build(const platf::hdr_frame_luminance_stats_t &stats) {
    if (!enabled()) {
      return nullptr;
    }
    // Validate before updating temporal state: a malformed GPU sample must
    // not replace a usable scene or poison its smoothing history.
    if (usable_scene_sample(stats, frame_.type, target_peak_nits_)) {
      last_scene_stats_ = stats;
    }
    if (!last_scene_stats_.valid) {
      return nullptr;
    }
    std::span<const std::uint8_t> payload;
    if (frame_.type == LI_PYROWAVE_METADATA_DOLBY_VISION_RPU) {
      const auto raw = video::dolby_vision::frame_metadata_from_stats(last_scene_stats_);
      if (!raw) {
        return nullptr;
      }
      const bool scene_refresh = scenes_.observe(last_scene_stats_);
      if (scene_refresh) {
        level1_filter_.reset();
      }
      // As in HEVC, smooth per encoded frame, but refresh only for a new scene.
      auto metadata = level1_filter_.update(*raw);
      metadata.scene_refresh = scene_refresh;
      payload = rpu_.generate(metadata);
    }
    else {
      const auto payloads = builder_.build(last_scene_stats_, target_peak_nits_);
      payload = frame_.type == LI_PYROWAVE_METADATA_HDR10_PLUS ? payloads.hdr10plus : payloads.vivid;
    }
    if (payload.empty()) {
      return nullptr;
    }
    frame_.payload.assign(payload.begin(), payload.end());
    if (format_ == static_cast<int>(hdr::dynamic_hdr_format_e::vivid_hlg)) {
      // UWA's HLG profile uses system_start_code 0x02 with the same PQ-domain
      // scene fields. Do not change the shared HEVC/PQ producer's payload.
      if (frame_.payload.size() < 6) return nullptr;
      frame_.payload[5] = 0x02;
    }
    return &frame_;
  }

}  // namespace pyrowave
