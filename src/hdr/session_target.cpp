/**
 * @file src/hdr/session_target.cpp
 * @brief Resolve one streaming session's effective HDR luminance target.
 */

#include "session_target.h"

#include "client_display_capabilities.h"
#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/rtsp.h"

#ifdef _WIN32
  #include "src/platform/windows/display_device/color_profile.h"
#endif

namespace hdr {
  void
  adopt_vdd_calibration_if_needed(rtsp_stream::launch_session_t &session) {
#ifdef _WIN32
    if (!session.enable_hdr || !session.use_vdd ||
        session.hdr_target_source != target_source_e::safe_defaults) return;

    const auto vdd_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
    if (vdd_id.empty()) return;
    const auto calibration = display_device::win_color_profile::current_hdr_calibration(vdd_id);
    if (!calibration) return;

    auto capabilities = session.hdr_capabilities;
    capabilities.reported = true;
    capabilities.max_nits = calibration->max_nits;
    capabilities.min_nits = calibration->min_nits;
    capabilities.max_full_frame_nits = calibration->max_full_frame_nits;
    session.set_hdr_target(capabilities, target_source_e::windows_hdr_calibration);
    BOOST_LOG(info) << "Using Windows HDR Calibration profile " << calibration->profile_name
                    << " for VDD HDR luminance";
#else
    (void) session;
#endif
  }

  void
  resolve_session_target(rtsp_stream::launch_session_t &session) {
    const auto target = resolve_effective_target(
      config::get_clients_config(),
      session.client_cert_uuid,
      session.client_name,
      session.reported_hdr_capabilities);
    if (!target) {
      BOOST_LOG(warning) << "Invalid per-client HDR brightness setting: " << target.error
                         << "; using " << to_string(target.source);
    }
    if (target.used_legacy_name) {
      BOOST_LOG(warning) << "Using legacy client-name matching for HDR brightness; pair the client again to bind by UUID";
    }

    session.set_hdr_target(target.capabilities, target.source);
    adopt_vdd_calibration_if_needed(session);
  }
}  // namespace hdr
