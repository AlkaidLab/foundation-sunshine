/**
 * @file src/hdr/session_target.h
 * @brief Resolve one streaming session's effective HDR luminance target.
 */
#pragma once

namespace rtsp_stream {
  struct launch_session_t;
}

namespace hdr {
  void
  resolve_session_target(rtsp_stream::launch_session_t &session);

  void
  adopt_vdd_calibration_if_needed(rtsp_stream::launch_session_t &session);
}  // namespace hdr
