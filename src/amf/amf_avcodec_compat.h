/**
 * @file src/amf/amf_avcodec_compat.h
 * @brief libavcodec AMF compatibility adapter for the standalone AMF encoder.
 */
#pragma once

#include "amf_config.h"

#include "src/video.h"

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>
#include <AMF/core/Data.h>
#include <AMF/core/Surface.h>

#include <deque>

namespace amf {

  struct amf_avcodec_compat_result {
    int hwsurfaces_in_queue_max = 16;
    bool manages_rate_control = true;
    AMF_RESULT result = AMF_OK;
  };

  struct amf_avcodec_scheduler_result {
    AMF_RESULT submit_result = AMF_OK;
    AMF_RESULT query_result = AMF_OK;
    ::amf::AMFDataPtr output_data;
    bool submitted = false;
    bool need_more_input = false;
    bool input_exhausted = false;
  };

  class amf_avcodec_scheduler {
  public:
    void
    configure(int hwsurfaces_in_queue_max, bool query_timeout_supported);

    void
    reset();

    int
    in_flight() const;

    amf_avcodec_scheduler_result
    submit_and_query(::amf::AMFComponent *encoder, const ::amf::AMFSurfacePtr &surface);

  private:
    AMF_RESULT
    query_once(::amf::AMFComponent *encoder, ::amf::AMFDataPtr &output_data);

    void
    sleep_if_needed(bool already_have_output) const;

    void
    decrement_in_flight();

    std::deque<::amf::AMFDataPtr> output_list;
    int hwsurfaces_in_queue = 0;
    int hwsurfaces_in_queue_max = 16;
    bool query_timeout_supported = false;
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
