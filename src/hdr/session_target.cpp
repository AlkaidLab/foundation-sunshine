/**
 * @file src/hdr/session_target.cpp
 * @brief Resolve one streaming session's effective HDR luminance target.
 */

#include "session_target.h"

#include "calibration.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/rtsp.h"

#ifdef _WIN32
  #include "src/display_device/vdd_utils.h"
  #include "src/platform/windows/display_device/color_profile.h"
#endif

namespace hdr {
  static std::optional<calibration_binding_t>
  bound_vdd_calibration(const std::string &uuid) {
#ifdef _WIN32
    if (config::video.vdd_reuse) return std::nullopt;
    auto binding = client_calibration(config::get_clients_config(), uuid);
    if (!binding) return std::nullopt;
    const auto installed = display_device::win_color_profile::read_hdr_calibration(binding->profile.profile_name);
    if (!installed.profile || !calibration_matches(*binding, installed.user_sid,
                                display_device::vdd_utils::generate_client_guid(uuid), *installed.profile)) {
      BOOST_LOG(warning) << "Ignoring saved HDR calibration for client " << uuid
                         << ": profile or ownership no longer matches; stage=" << installed.stage
                         << ", HRESULT=" << installed.error;
      return std::nullopt;
    }
    binding->profile = *installed.profile;
    return binding;
#else
    (void) uuid;
    return std::nullopt;
#endif
  }

  bool
  apply_vdd_calibration(const rtsp_stream::launch_session_t &session, const std::string &device_id) {
#ifdef _WIN32
    if (!session.enable_hdr || !session.use_vdd) return true;
    if (const auto binding = bound_vdd_calibration(session.client_cert_uuid)) {
      const auto brightness = [](const client_display_capabilities_t &value) {
        return display_device::vdd_utils::hdr_brightness_t { value.max_nits, value.min_nits, value.max_full_frame_nits };
      };
      if ((session.hdr_target_source == target_source_e::windows_hdr_calibration ||
            session.hdr_target_source == target_source_e::safe_defaults) &&
          brightness(session.hdr_capabilities) != brightness(binding->profile.luminance)) {
        // A profile may become available or be replaced while Windows is locked.
        // The active VDD still advertises the values selected at session start.
        BOOST_LOG(warning) << "HDR calibration luminance changed since display preparation; reconnect to apply it";
        return false;
      }
      return display_device::win_color_profile::apply_hdr_calibration(device_id, *binding);
    }
    return session.hdr_target_source != target_source_e::windows_hdr_calibration;
#else
    (void) session;
    (void) device_id;
    return true;
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
    // Resolve ownership before any VDD exists or is replaced. Looking up the
    // currently visible Zako display here can import the previous client's ICC.
    if (session.enable_hdr && session.use_vdd && target.source == target_source_e::safe_defaults) {
      if (const auto binding = bound_vdd_calibration(session.client_cert_uuid)) {
        session.set_hdr_target(binding->profile.luminance, target_source_e::windows_hdr_calibration);
        BOOST_LOG(info) << "Using saved HDR calibration " << binding->profile.profile_name << " for client " << session.client_cert_uuid;
      }
    }
  }
}  // namespace hdr
