/**
 * @file tests/unit/test_amf_rate_control.cpp
 * @brief Locks the AMF rate-control derivation shared by both encoder paths
 * (standalone and avcodec-compatible). An unset amd_rc must resolve to a hard
 * CBR constraint — client requests always carry peak == target, and the
 * driver's soft-target Latency Constrained VBR default otherwise lets bursts
 * blow past the client's bitrate target — and an explicit amd_rc must pass
 * through unchanged. The VBV budget stays at one frame of bitrate.
 */
#include "src/amf/amf_avcodec_compat.h"

#include <gtest/gtest.h>

#include <AMF/components/VideoEncoderAV1.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderVCE.h>

namespace {

  class amf_rate_control_test: public ::testing::Test {
  protected:
    video::config_t client_config;

    void
    SetUp() override {
      client_config = video::config_t {};
      client_config.framerate = 60;
      client_config.bitrate = 20000;
    }
  };

  TEST_F(amf_rate_control_test, unset_rc_defaults_to_cbr_on_every_codec) {
    const amf::amf_config config;

    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_h264(config, client_config), AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_hevc(config, client_config), AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_av1(config, client_config), AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR);
  }

  TEST_F(amf_rate_control_test, explicit_rc_mode_passes_through) {
    amf::amf_config config;
    // display_vram.cpp resolves amd_rc_* into amf_config.rc_mode per codec, so
    // each derivation must pass through its own codec's enumerator unchanged.
    config.rc_mode = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR;
    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_h264(config, client_config), AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);

    config.rc_mode = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR;
    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_hevc(config, client_config), AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);

    config.rc_mode = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR;
    EXPECT_EQ(amf::amf_avcodec_compat::rate_control_av1(config, client_config), AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);
  }

  TEST_F(amf_rate_control_test, vbv_budget_is_one_frame_of_bitrate) {
    const auto expected = static_cast<int64_t>(client_config.bitrate) * 1000 / 60;
    EXPECT_EQ(amf::amf_avcodec_compat::vbv_buffer_size(client_config.bitrate, client_config), expected);

    client_config.frameRateNum = 120;
    client_config.frameRateDen = 1;
    EXPECT_EQ(amf::amf_avcodec_compat::vbv_buffer_size(client_config.bitrate, client_config),
      static_cast<int64_t>(client_config.bitrate) * 1000 / 120);
  }

  TEST_F(amf_rate_control_test, vbv_budget_survives_degenerate_framerate) {
    client_config.framerate = 0;
    client_config.frameRateNum = 0;

    EXPECT_EQ(amf::amf_avcodec_compat::vbv_buffer_size(client_config.bitrate, client_config),
      static_cast<int64_t>(client_config.bitrate) * 1000);
  }

}  // namespace
