/**
 * @file src/amf/amf_avcodec_compat.h
 * @brief libavcodec AMF compatibility adapter for the standalone AMF encoder.
 */
#pragma once

#include "amf_config.h"

#include "src/video.h"

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>

namespace amf {

  struct amf_avcodec_compat_result {
    int hwsurfaces_in_queue_max = 16;
    bool manages_rate_control = true;
  };

  class amf_avcodec_compat {
  public:
    static amf_avcodec_compat_result
    configure(::amf::AMFComponent *encoder,
      int video_format,
      const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace);

    static int64_t
    vbv_buffer_size(int bitrate_kbps, const video::config_t &client_config);
  };

}  // namespace amf
