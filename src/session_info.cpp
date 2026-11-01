/**
 * @file src/session_info.cpp
 * @brief JSON helpers for the session information returned by the session APIs.
 */
#include "session_info.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

namespace stream {

  std::string
  codec_name(int video_format) {
    switch (video_format) {
      case 0:
        return "h264";
      case 1:
        return "hevc";
      case 2:
        return "av1";
      default:
        break;
    }
    if (video_format >= 0 && static_cast<std::uint32_t>(video_format) == LI_PYROWAVE_VIDEO_FORMAT) {
      return "pyrowave";
    }
    return {};
  }

  void
  add_session_client_json(nlohmann::json &session_obj, const session_info_t &session_info) {
    session_obj["client_family"] = session_info.client_family.empty() ? std::string("unknown") : session_info.client_family;
    session_obj["client_official"] = session_info.client_official ? nlohmann::json(*session_info.client_official) : nlohmann::json(nullptr);
    session_obj["client_os"] = session_info.client_os.empty() ? nlohmann::json(nullptr) : nlohmann::json(session_info.client_os);
    session_obj["client_version"] = session_info.client_version.empty() ? nlohmann::json(nullptr) : nlohmann::json(session_info.client_version);
  }

  void
  add_session_quality_json(nlohmann::json &session_obj, const session_info_t &session_info) {
    session_obj["video_format"] = session_info.video_format;
    session_obj["codec"] = session_info.codec;
    session_obj["packet_loss"] = session_info.packet_loss ? nlohmann::json(*session_info.packet_loss) : nlohmann::json(nullptr);
    session_obj["dropped_frames"] = session_info.dropped_frames ? nlohmann::json(*session_info.dropped_frames) : nlohmann::json(nullptr);
  }

  void
  add_session_gamepad_json(nlohmann::json &session_obj, const session_info_t &session_info) {
    session_obj["gamepad_type"] = session_info.gamepad_type;
    session_obj["gamepad_dualsense_identity"] = session_info.gamepad_dualsense_identity;
  }

}  // namespace stream
