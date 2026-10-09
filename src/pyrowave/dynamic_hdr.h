#pragma once

#include "src/platform/common.h"
#include "src/video_dolby_vision.h"
#include "src/video_hdr_metadata.h"

#include <cstdint>
#include <vector>

namespace pyrowave {

  struct hdr_frame_metadata_t {
    std::uint16_t type = 0;
    std::uint16_t hlg_nominal_peak_nits = 0;
    std::vector<std::uint8_t> payload;
  };

  class hdr_metadata_producer_t {
  public:
    bool configure(int format, const SS_HDR_METADATA &source, std::uint16_t target_peak_nits);
    bool enabled() const noexcept { return format_ != 0; }

    // The result is owned here until the next build. Transport copies it into
    // this frame's protected payload before the encoder advances.
    const hdr_frame_metadata_t *build(const platf::hdr_frame_luminance_stats_t &stats);

  private:
    int format_ = 0;
    std::uint16_t target_peak_nits_ = 1000;
    std::uint64_t last_sample_sequence_ = 0;
    bool has_dv_sample_ = false;
    video::hdr_metadata::dynamic_metadata_builder_t builder_;
    video::hdr_metadata::scene_change_detector_t scenes_;
    video::dolby_vision::rpu_generator_t rpu_;
    video::dolby_vision::level1_temporal_filter_t level1_filter_;
    video::dolby_vision::frame_metadata_t last_dv_sample_ {};
    hdr_frame_metadata_t frame_;
  };

}  // namespace pyrowave
