/**
 * @file src/session_info.h
 * @brief Read-only description of an active streaming session for API responses.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace stream {

  // Session information structure for API responses
  struct session_info_t {
    std::string client_name;
    std::string client_uuid;
    std::string client_address;
    std::string state;
    std::string stop_reason;
    uint32_t session_id;
    std::int64_t uptime_ms;
    std::int64_t control_idle_ms;
    std::int64_t video_idle_ms;
    std::int64_t audio_idle_ms;
    bool control_connected;
    int width;
    int height;
    int fps;
    int bitrate;  // Session budget in Kbps (encoder + audio + FEC), not a measured rate
    int measured_bitrate_kbps = 0;  // UDP payload Kbps over the last ~2s; 0 when that window is empty
    bool host_audio;
    bool enable_hdr;
    bool enable_mic;
    bool use_vdd;
    bool hdr_brightness_reported;
    std::string hdr_brightness_source;
    float hdr_max_nits;
    float hdr_min_nits;
    float hdr_max_full_frame_nits;
    std::string app_name;
    int app_id;

    /// Client family id of the paired device (docs/api/client-family.md); empty when unknown.
    std::string client_family;
    /// True for an AlkaidLab client, false for a third-party one; empty when the family is unknown.
    std::optional<bool> client_official;
    /// Operating system of the client ("macos", "windows", ...); empty when unknown.
    std::string client_os;
    /// Product version the client reported; empty when unknown.
    std::string client_version;

    /// Negotiated wire video format (0 H.264, 1 HEVC, 2 AV1, experimental formats above); -1 when unknown.
    int video_format = -1;
    /// Name of video_format ("h264", "hevc", "av1", "pyrowave"); empty when unknown.
    std::string codec;
    /// Latest client-reported packet loss in percent; empty when the session has reported none.
    std::optional<double> packet_loss;
    /// Latest client-reported dropped frame count; empty when the session has reported none.
    std::optional<int> dropped_frames;
    /// Smoothed round-trip time of the control channel in ms; empty when there is no control connection or no measurement yet.
    std::optional<int> rtt_ms;

    /// Active virtual pad type of the client's PlayStation controller ("ds4", "ds5", "x360");
    /// empty when the client has no PlayStation controller or the platform does not name its pads.
    std::string gamepad_type;
    /// The client's arrival metadata identified that controller as a DualSense.
    bool gamepad_dualsense_identity = false;
  };

  /**
   * @brief Map a wire video format to its codec name.
   * @return "h264", "hevc", "av1" or "pyrowave"; an empty string for any other value (including -1).
   */
  std::string
  codec_name(int video_format);

  /**
   * @brief Add the video_format, codec, packet_loss and dropped_frames keys to a session JSON object.
   *
   * video_format is always an integer (-1 when unknown). codec is always a string. packet_loss and
   * dropped_frames are JSON null when the session has reported no value, so a reader can tell
   * "not reported" from 0.
   */
  /**
   * @brief Add the client_family, client_official, client_os and client_version keys to a session JSON object.
   *
   * client_family is always a string ("unknown" when no rule matched). The other three are JSON null when unknown.
   */
  void
  add_session_client_json(nlohmann::json &session_obj, const session_info_t &session_info);

  void
  add_session_quality_json(nlohmann::json &session_obj, const session_info_t &session_info);

  /**
   * @brief Add the gamepad_type and gamepad_dualsense_identity keys to a session JSON object.
   *
   * Both keys are always present: an empty string and false mean the client has no PlayStation
   * controller allocated (or the platform does not name its virtual pads), so a reader can tell
   * "no pad" from "ds4".
   */
  void
  add_session_gamepad_json(nlohmann::json &session_obj, const session_info_t &session_info);

}  // namespace stream
