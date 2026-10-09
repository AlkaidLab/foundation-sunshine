#include "dynamic_hdr.h"

#include "src/hdr/dynamic_hdr_selection.h"
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"

#include <algorithm>
#include <span>

namespace pyrowave {

  bool
  hdr_metadata_producer_t::configure(int format, const SS_HDR_METADATA &source, std::uint16_t target_peak_nits) {
    format_ = 0;
    frame_ = {};
    builder_.reset();
    scenes_.reset();
    rpu_.reset();
    level1_filter_.reset();
    last_sample_sequence_ = 0;
    has_dv_sample_ = false;
    last_dv_sample_ = {};
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
    if (!enabled() || !stats.valid) {
      return nullptr;
    }
    std::span<const std::uint8_t> payload;
    if (frame_.type == LI_PYROWAVE_METADATA_DOLBY_VISION_RPU) {
      const bool new_sample = !has_dv_sample_ || stats.sample_sequence == 0 ||
                              stats.sample_sequence != last_sample_sequence_;
      if (new_sample) {
        const auto raw = video::dolby_vision::frame_metadata_from_stats(stats);
        if (!raw) {
          return nullptr;
        }
        const bool scene_refresh = scenes_.observe(stats);
        if (scene_refresh) {
          level1_filter_.reset();
        }
        last_dv_sample_ = level1_filter_.update(*raw);
        last_dv_sample_.scene_refresh = scene_refresh;
        last_sample_sequence_ = stats.sample_sequence;
        has_dv_sample_ = true;
      }
      else {
        last_dv_sample_.scene_refresh = false;
      }
      payload = rpu_.generate(last_dv_sample_);
    }
    else {
      const auto payloads = builder_.build(stats, target_peak_nits_);
      payload = frame_.type == LI_PYROWAVE_METADATA_HDR10_PLUS ? payloads.hdr10plus : payloads.vivid;
    }
    if (payload.empty()) {
      return nullptr;
    }
    frame_.payload.assign(payload.begin(), payload.end());
    return &frame_;
  }

}  // namespace pyrowave
