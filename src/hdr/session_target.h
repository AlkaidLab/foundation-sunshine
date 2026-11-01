/**
 * @file src/hdr/session_target.h
 * @brief Resolve one streaming session's effective HDR luminance target.
 */
#pragma once

#include "calibration.h"

namespace rtsp_stream {
  struct launch_session_t;
}

namespace hdr {
  void
  resolve_session_target(rtsp_stream::launch_session_t &session);

  /** Verified, user-owned calibration for this client's dedicated VDD only. */
  std::optional<calibration_binding_t>
  bound_vdd_calibration(const std::string &uuid);

  /** Restore the prepared VDD's verified ICC without changing display topology. */
  bool
  apply_vdd_calibration(const rtsp_stream::launch_session_t &session, const std::string &device_id);
}  // namespace hdr
