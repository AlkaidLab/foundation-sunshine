/**
 * @file src/amf/amf_d3d11.h
 * @brief Declarations for AMF D3D11 encoder.
 */
#pragma once

#include "amf_encoder.h"

#include <d3d11.h>
#include <memory>
#include <string>

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>
#include <AMF/core/Factory.h>

namespace amf {

  /**
   * @brief AMF encoder using D3D11 for texture input.
   */
  class amf_d3d11: public amf_encoder {
  public:
    explicit amf_d3d11(ID3D11Device *d3d_device);
    ~amf_d3d11();

    bool
    create_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format) override;

    void
    destroy_encoder() override;

    amf_encoded_frame
    encode_frame(uint64_t frame_index, bool force_idr) override;

    bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) override;

    void
    set_bitrate(int bitrate_kbps) override;

    void
    set_hdr_metadata(const std::optional<amf_hdr_metadata> &metadata) override;

    void *
    get_input_texture() override;

  private:
    bool
    init_amf_library();

    bool
    configure_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace);

    AMF_SURFACE_FORMAT
    get_amf_format(platf::pix_fmt_e buffer_format, int bit_depth);

    const wchar_t *
    get_codec_id();

    bool
    set_ltr_property(const wchar_t *name, int64_t value);

    template<typename T>
    void
    set_codec_property(const wchar_t *h264_name, const wchar_t *hevc_name, const wchar_t *av1_name, T value);

    ID3D11Device *device = nullptr;
    ::amf::AMFFactory *factory = nullptr;
    ::amf::AMFContextPtr context;
    ::amf::AMFComponentPtr encoder;
    HMODULE amf_dll = nullptr;

    // Input texture that the rendering pipeline writes to
    ID3D11Texture2D *input_texture = nullptr;

    // Encoder state
    video::config_t current_config {};
    int video_format = 0;  // 0=H264, 1=HEVC, 2=AV1
    bool rfi_pending = false;
    uint64_t last_rfi_ltr_index = 0;
    int max_ltr_frames = 0;

    // Current LTR state for RFI
    static constexpr int MAX_LTR_SLOTS = 2;
    static constexpr uint64_t LTR_MARK_INTERVAL = 30;  // Mark LTR every N frames
    int current_ltr_slot = 0;      // Which LTR slot to mark next
    bool ltr_slots_valid[MAX_LTR_SLOTS] = {};
    uint64_t ltr_slot_frame_index[MAX_LTR_SLOTS] = {};  // Frame index when each LTR slot was marked

    // Statistics feedback state
    bool statistics_enabled = false;
    bool psnr_enabled = false;
    bool ssim_enabled = false;

    std::string last_error_string;
  };

  /**
   * @brief Create an AMF D3D11 encoder instance.
   * @param d3d_device The D3D11 device to use.
   * @return AMF encoder or nullptr on failure.
   */
  std::unique_ptr<amf_d3d11>
  create_amf_d3d11(ID3D11Device *d3d_device);

}  // namespace amf
