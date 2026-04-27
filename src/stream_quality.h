/**
 * @file src/stream_quality.h
 * @brief Low-bitrate screen clarity planning helpers.
 */
#pragma once

namespace stream_quality {

  enum class content_type_e {
    desktop,
    text,
    motion,
    game,
  };

  struct stream_description_t {
    int width = 0;
    int height = 0;
    int fps = 0;
    int video_bitrate_kbps = 0;
    int video_format = 0;  // 0 = H.264, 1 = HEVC, 2 = AV1
    int chroma_sampling_type = 0;  // 0 = 4:2:0, 1 = 4:4:4
    content_type_e content_type = content_type_e::desktop;
  };

  struct clarity_plan_t {
    bool enabled = false;
    int video_bitrate_kbps = 0;
    int effective_fps = 0;
    int effective_chroma_sampling_type = 0;
    double bits_per_pixel_per_frame = 0.0;
    int target_qp = 0;
    bool roi_enabled = false;
    bool prefer_long_term_reference = false;
    bool prefer_intra_refresh = false;
    float sharpen_alpha = 0.0f;
  };

  double
  bits_per_pixel_per_frame(const stream_description_t &stream);

  clarity_plan_t
  plan_low_bitrate_clarity(const stream_description_t &stream);

  int
  startup_bitrate_for_ceiling(const stream_description_t &stream);

  int
  startup_fps_for_bitrate(const stream_description_t &stream, int startup_bitrate_kbps);

}  // namespace stream_quality
