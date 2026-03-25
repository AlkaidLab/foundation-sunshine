/**
 * @file src/amf/amf_encoder.h
 * @brief Declarations for standalone AMF encoder interface.
 */
#pragma once

#include "amf_config.h"
#include "amf_encoded_frame.h"

#include "src/platform/common.h"
#include "src/video.h"
#include "src/video_colorspace.h"

namespace amf {

  /**
   * @brief Standalone AMF encoder interface.
   */
  class amf_encoder {
  public:
    virtual ~amf_encoder() = default;

    /**
     * @brief Create the encoder.
     * @param config AMF encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    virtual bool
    create_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format) = 0;

    /**
     * @brief Destroy the encoder.
     */
    virtual void
    destroy_encoder() = 0;

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Unique frame identifier.
     * @param force_idr Whether to encode frame as forced IDR.
     * @return Encoded frame.
     */
    virtual amf_encoded_frame
    encode_frame(uint64_t frame_index, bool force_idr) = 0;

    /**
     * @brief Perform reference frame invalidation (RFI).
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error (caller should force IDR).
     */
    virtual bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) = 0;

    /**
     * @brief Set the bitrate for the encoder dynamically.
     * @param bitrate_kbps Bitrate in kilobits per second.
     */
    virtual void
    set_bitrate(int bitrate_kbps) = 0;

    /**
     * @brief Set HDR metadata for the encoder.
     * @param metadata HDR metadata, or nullopt to disable.
     */
    virtual void
    set_hdr_metadata(const std::optional<amf_hdr_metadata> &metadata) = 0;

    /**
     * @brief Get the D3D11 input texture the encoder reads from.
     * @return Pointer to ID3D11Texture2D, or nullptr.
     */
    virtual void *
    get_input_texture() = 0;
  };

}  // namespace amf
