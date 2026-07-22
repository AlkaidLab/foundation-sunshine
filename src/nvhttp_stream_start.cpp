/**
 * @file src/nvhttp_stream_start.cpp
 * @brief Stream startup diagnostics and low-risk recovery for GameStream launch/resume.
 */

// standard includes
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

// local includes
#include "nvhttp_stream_start.h"

#include "config.h"
#include "display_device/parsed_config.h"
#include "display_device/session.h"
#include "globals.h"
#include "logging.h"
#include "video.h"

namespace nvhttp::stream_start {

  namespace pt = boost::property_tree;

  namespace {

    struct auto_recovery_result_t {
      bool attempted { false };
      bool succeeded { false };
      std::string action;
      std::string detail;
    };

    class temporary_video_config_t {
    public:
      // This is only used while preparing a launch when no active RTSP session is
      // running, so the temporary global config swap cannot race stream encoding.
      explicit temporary_video_config_t(config::video_t replacement):
          original_config { config::video } {
        config::video = std::move(replacement);
      }

      ~temporary_video_config_t() {
        config::video = std::move(original_config);
      }

      temporary_video_config_t(const temporary_video_config_t &) = delete;
      temporary_video_config_t &
      operator=(const temporary_video_config_t &) = delete;

    private:
      config::video_t original_config;
    };

    void
    set_auto_recovery_status(pt::ptree &tree, const auto_recovery_result_t &recovery_result) {
      if (!recovery_result.attempted) {
        return;
      }

      tree.put("root.sunshine_auto_recovery_attempted", 1);
      tree.put("root.sunshine_auto_recovery_action", recovery_result.action);
      tree.put("root.sunshine_auto_recovery_result", recovery_result.succeeded ? "succeeded" : "failed");
      tree.put("root.sunshine_auto_recovery_detail", recovery_result.detail);
    }

    std::string
    video_probe_error_code(video::probe_error_e error) {
      switch (error) {
        case video::probe_error_e::no_active_display:
          return "NO_ACTIVE_DISPLAY";
        case video::probe_error_e::configured_encoder_unavailable:
          return "CONFIGURED_ENCODER_UNAVAILABLE";
        case video::probe_error_e::codec_requirements_unmet:
          return "CODEC_REQUIREMENTS_UNMET";
        case video::probe_error_e::no_working_encoder:
          return "NO_WORKING_ENCODER";
        case video::probe_error_e::none:
        default:
          return "VIDEO_INITIALIZATION_FAILED";
      }
    }

    std::string
    display_config_error_code(display_device::session_t::configure_result_t::result_e result) {
      using result_e = display_device::session_t::configure_result_t::result_e;

      switch (result) {
        case result_e::vdd_not_installed:
          return "VDD_DRIVER_NOT_INSTALLED";
        case result_e::vdd_unavailable:
          return "VDD_DRIVER_UNAVAILABLE";
        case result_e::vdd_create_failed:
          return "VDD_CREATE_FAILED";
        case result_e::parse_fail:
          return "DISPLAY_CONFIG_PARSE_FAILED";
        case result_e::topology_fail:
          return "DISPLAY_TOPOLOGY_FAILED";
        case result_e::primary_display_fail:
          return "DISPLAY_PRIMARY_FAILED";
        case result_e::modes_fail:
          return "DISPLAY_MODE_FAILED";
        case result_e::hdr_states_fail:
          return "DISPLAY_HDR_FAILED";
        case result_e::file_save_fail:
          return "DISPLAY_PERSISTENCE_SAVE_FAILED";
        case result_e::revert_fail:
          return "DISPLAY_REVERT_FAILED";
        case result_e::deferred_retry:
          return "DISPLAY_CONFIG_DEFERRED";
        case result_e::success:
        default:
          return "DISPLAY_CONFIG_FAILED";
      }
    }

    void
    set_display_config_error(pt::ptree &tree, const display_device::session_t::configure_result_t &display_result) {
      const std::string status_message = display_result.message.empty() ?
                                           "Sunshine could not apply the requested display configuration." :
                                           display_result.message;
      const std::string hint = display_result.hint.empty() ?
                                 "Set display, VDD, resolution, refresh rate, and HDR options to Auto, then try again." :
                                 display_result.hint;

      set_sunshine_error(
        tree,
        503,
        status_message,
        display_config_error_code(display_result.result),
        hint,
        "review_display_settings",
        "display",
        "display_configuration",
        true);
    }

    void
    set_video_probe_error(pt::ptree &tree) {
      const auto &probe_result = video::last_encoder_probe_result;
      const std::string status_message = probe_result.message.empty() ?
                                           "Sunshine could not initialize display capture or video encoding." :
                                           probe_result.message;
      const std::string hint = probe_result.hint.empty() ?
                                 "Check that a display or VDD is active, set GPU/display/encoder options to Auto, and try again." :
                                 probe_result.hint;

      set_sunshine_error(
        tree,
        503,
        status_message,
        video_probe_error_code(probe_result.error),
        hint,
        "review_video_display_settings",
        "video",
        "encoder_probe",
        true);
    }

    bool
    retry_deferred_display_config(
      const rtsp_stream::launch_session_t &launch_session,
      bool is_reconfigure,
      display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result) {
      if (display_result.result != display_device::session_t::configure_result_t::result_e::deferred_retry) {
        return false;
      }

      recovery_result = {
        true,
        false,
        "deferred_display_retry",
        "Display configuration was deferred; retrying briefly before returning an error."
      };

      constexpr std::array retry_delays {
        std::chrono::milliseconds { 500 },
        std::chrono::milliseconds { 1000 },
        std::chrono::milliseconds { 1500 }
      };

      for (const auto delay : retry_delays) {
        std::this_thread::sleep_for(delay);
        display_result = display_device::session_t::get().configure_display(config::video, launch_session, is_reconfigure);
        if (!video::probe_encoders()) {
          recovery_result.succeeded = true;
          recovery_result.detail = "Display configuration became available after a short retry.";
          BOOST_LOG(info) << "Recovered stream startup after deferred display configuration retry";
          return true;
        }
      }

      recovery_result.detail = "Display configuration was still unavailable after short retries.";
      return false;
    }

    bool
    should_try_vdd_for_display_config(display_device::session_t::configure_result_t::result_e result) {
      using result_e = display_device::session_t::configure_result_t::result_e;

      switch (result) {
        case result_e::topology_fail:
        case result_e::primary_display_fail:
        case result_e::modes_fail:
        case result_e::hdr_states_fail:
        case result_e::vdd_not_installed:
        case result_e::vdd_unavailable:
        case result_e::vdd_create_failed:
          return true;
        case result_e::success:
        case result_e::deferred_retry:
        case result_e::parse_fail:
        case result_e::file_save_fail:
        case result_e::revert_fail:
        default:
          return false;
      }
    }

    display_device::parsed_config_t::device_prep_e
    effective_device_prep_for_launch(const rtsp_stream::launch_session_t &launch_session) {
      using device_prep_e = display_device::parsed_config_t::device_prep_e;

      const auto configured_device_prep = static_cast<device_prep_e>(config::video.display_device_prep);
      if (launch_session.custom_screen_mode < 0) {
        return configured_device_prep;
      }

      const auto custom_screen_mode = static_cast<device_prep_e>(launch_session.custom_screen_mode);
      switch (custom_screen_mode) {
        case device_prep_e::no_operation:
        case device_prep_e::ensure_active:
        case device_prep_e::ensure_primary:
        case device_prep_e::ensure_only_display:
        case device_prep_e::ensure_secondary:
          return custom_screen_mode;
        default:
          return configured_device_prep;
      }
    }

    bool
    display_no_operation_requested(const rtsp_stream::launch_session_t &launch_session) {
      return effective_device_prep_for_launch(launch_session) == display_device::parsed_config_t::device_prep_e::no_operation;
    }

    bool
    explicit_vdd_requested_for_launch(const rtsp_stream::launch_session_t &launch_session) {
      const auto display_request = display_device::resolve_display_request(config::video, launch_session);
      const bool is_vdd_device = display_device::get_display_friendly_name(display_request.device_id) == ZAKO_NAME;
      return display_request.use_vdd || is_vdd_device;
    }

    bool
    no_operation_blocks_automatic_vdd_recovery(const rtsp_stream::launch_session_t &launch_session) {
      return display_no_operation_requested(launch_session) && !explicit_vdd_requested_for_launch(launch_session);
    }

    bool
    display_config_failure_can_continue_with_current_display(display_device::session_t::configure_result_t::result_e result) {
      using result_e = display_device::session_t::configure_result_t::result_e;
      switch (result) {
        case result_e::modes_fail:
        case result_e::hdr_states_fail:
          return true;
        case result_e::topology_fail:
        case result_e::primary_display_fail:
        case result_e::success:
        case result_e::deferred_retry:
        case result_e::vdd_not_installed:
        case result_e::vdd_unavailable:
        case result_e::vdd_create_failed:
        case result_e::parse_fail:
        case result_e::file_save_fail:
        case result_e::revert_fail:
        default:
          return false;
      }
    }

    void
    fill_vdd_recovery_session(rtsp_stream::launch_session_t &recovery_session,
      const rtsp_stream::launch_session_t &launch_session) {
      // launch_session_t is not copy-assignable because RTSP cipher state is
      // move-only, so copy only the launch fields needed for display probing.
      recovery_session.id = launch_session.id;
      recovery_session.gcm_key = launch_session.gcm_key;
      recovery_session.iv = launch_session.iv;
      recovery_session.av_ping_payload = launch_session.av_ping_payload;
      recovery_session.control_connect_data = launch_session.control_connect_data;
      recovery_session.env = launch_session.env;
      recovery_session.host_audio = launch_session.host_audio;
      recovery_session.unique_id = launch_session.unique_id;
      recovery_session.client_name = launch_session.client_name;
      recovery_session.width = launch_session.width;
      recovery_session.height = launch_session.height;
      recovery_session.fps = launch_session.fps;
      recovery_session.gcmap = launch_session.gcmap;
      recovery_session.appid = launch_session.appid;
      recovery_session.surround_info = launch_session.surround_info;
      recovery_session.surround_params = launch_session.surround_params;
      recovery_session.continuous_audio = launch_session.continuous_audio;
      recovery_session.enable_hdr = launch_session.enable_hdr;
      recovery_session.enable_sops = launch_session.enable_sops;
      recovery_session.enable_mic = launch_session.enable_mic;
      recovery_session.use_vdd = true;
      recovery_session.custom_screen_mode = launch_session.custom_screen_mode;
      recovery_session.max_nits = launch_session.max_nits;
      recovery_session.min_nits = launch_session.min_nits;
      recovery_session.max_full_nits = launch_session.max_full_nits;
      recovery_session.rtsp_url_scheme = launch_session.rtsp_url_scheme;
      recovery_session.rtsp_iv_counter = launch_session.rtsp_iv_counter;
      recovery_session.setup_video = launch_session.setup_video;
      recovery_session.setup_audio = launch_session.setup_audio;
      recovery_session.setup_control = launch_session.setup_control;
      recovery_session.setup_mic = launch_session.setup_mic;
      recovery_session.control_only = launch_session.control_only;
      recovery_session.env["SUNSHINE_CLIENT_USE_VDD"] = "true";
    }

    void
    commit_vdd_recovery_to_launch_session(rtsp_stream::launch_session_t &launch_session) {
      launch_session.use_vdd = true;
      launch_session.env["SUNSHINE_CLIENT_USE_VDD"] = "true";
      launch_session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = config::video.output_name;
    }

    bool
    recover_display_with_vdd(
      rtsp_stream::launch_session_t &launch_session,
      bool is_reconfigure,
      display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result) {
      const bool no_active_display = video::last_encoder_probe_result.error == video::probe_error_e::no_active_display;
      const bool display_config_mismatch = should_try_vdd_for_display_config(display_result.result);
      if (!no_active_display && !display_config_mismatch) {
        return false;
      }

      if (no_operation_blocks_automatic_vdd_recovery(launch_session)) {
        recovery_result = {
          true,
          false,
          "vdd_display_recovery_skipped",
          "Display preparation is no_operation; skipping automatic VDD recovery to avoid switching display targets."
        };
        BOOST_LOG(info) << "Skipping automatic VDD recovery because display preparation is no_operation";
        return false;
      }

      recovery_result = {
        true,
        false,
        "vdd_display_recovery",
        display_config_mismatch ?
          "The physical display could not satisfy the requested stream display settings; trying a VDD-backed display." :
          "No active display was available; trying a VDD-backed display recovery."
      };

      const auto original_display_result = display_result;
      rtsp_stream::launch_session_t recovery_session {};
      fill_vdd_recovery_session(recovery_session, launch_session);

      display_result = display_device::session_t::get().configure_display(config::video, recovery_session, is_reconfigure);
      if (display_result.result != display_device::session_t::configure_result_t::result_e::success) {
        recovery_result.detail = "VDD-backed display recovery was attempted, but the VDD display configuration failed.";
        return false;
      }

      if (!video::probe_encoders()) {
        commit_vdd_recovery_to_launch_session(launch_session);
        recovery_result.succeeded = true;
        recovery_result.detail = "A VDD-backed display became available and encoder probing succeeded.";
        BOOST_LOG(info) << "Recovered stream startup by preparing a VDD-backed display";
        return true;
      }

      display_result = original_display_result;
      recovery_result.detail = "VDD-backed display recovery was attempted, but encoder probing still failed.";
      return false;
    }

    bool
    recover_with_temporary_encoder_config(auto_recovery_result_t &recovery_result) {
      const auto probe_error = video::last_encoder_probe_result.error;
      if (probe_error != video::probe_error_e::configured_encoder_unavailable &&
          probe_error != video::probe_error_e::codec_requirements_unmet) {
        return false;
      }

      const auto original_probe_result = video::last_encoder_probe_result;
      auto fallback_config = config::video;
      if (probe_error == video::probe_error_e::configured_encoder_unavailable) {
        fallback_config.encoder.clear();
        recovery_result = {
          true,
          false,
          "temporary_auto_encoder_fallback",
          "The configured encoder was unavailable; trying Auto encoder for this startup."
        };
      }
      else {
        fallback_config.hevc_mode = 1;
        fallback_config.av1_mode = 1;
        recovery_result = {
          true,
          false,
          "temporary_h264_fallback",
          "The requested HEVC/AV1 requirements were unavailable; trying H.264 for this startup."
        };
      }

      {
        temporary_video_config_t temporary_config { std::move(fallback_config) };
        if (!video::probe_encoders()) {
          recovery_result.succeeded = true;
          recovery_result.detail = "Temporary encoder fallback succeeded without changing the saved configuration.";
          BOOST_LOG(info) << "Recovered stream startup using " << recovery_result.action;
          return true;
        }
      }

      recovery_result.detail = "Temporary encoder fallback was attempted, but encoder probing still failed.";
      video::last_encoder_probe_result = original_probe_result;
      return false;
    }

  }  // namespace

  void
  set_sunshine_error(
    pt::ptree &tree,
    int status_code,
    const std::string &status_message,
    const std::string &error_code,
    const std::string &hint,
    const std::string &recovery_action,
    const std::string &source,
    const std::string &stage,
    bool recoverable) {
    std::string client_status_message = status_message;
    if (!hint.empty() && client_status_message.find(hint) == std::string::npos) {
      client_status_message += " ";
      client_status_message += hint;
    }

    tree.put("root.<xmlattr>.status_code", status_code);
    tree.put("root.<xmlattr>.status_message", client_status_message);
    tree.put("root.sunshine_error_code", error_code);
    tree.put("root.sunshine_error_hint", hint);
    tree.put("root.sunshine_recovery_action", recovery_action);
    tree.put("root.sunshine_error_source", source);
    tree.put("root.sunshine_error_stage", stage);
    tree.put("root.sunshine_recoverable", recoverable ? 1 : 0);
  }

  bool
  prepare_display_and_probe_encoders(
    pt::ptree &tree,
    rtsp_stream::launch_session_t &launch_session,
    bool is_reconfigure) {
    // Display configuration can change the active capture target, so probe
    // encoders only after the display stack has settled.
    auto display_result = display_device::session_t::get().configure_display(config::video, launch_session, is_reconfigure);
    auto_recovery_result_t recovery_result;
    const auto should_try_vdd = should_try_vdd_for_display_config(display_result.result);

    if (should_try_vdd &&
        display_config_failure_can_continue_with_current_display(display_result.result)) {
      recovery_result = {
        true,
        false,
        "current_display_settings_fallback",
        "Display mode or HDR configuration failed; probing the current display state before using fallback recovery."
      };

      BOOST_LOG(warning) << "Display mode/HDR configuration failed; continuing with current display settings if encoder probing succeeds";
      if (!video::probe_encoders()) {
        recovery_result.succeeded = true;
        recovery_result.detail = "Encoder probing succeeded with the current display settings.";
        set_auto_recovery_status(tree, recovery_result);
        return true;
      }

      recovery_result.detail = "The current display settings were kept, but encoder probing still failed.";
      if (display_result.cleanup_on_failure) {
        display_device::session_t::get().restore_state();
      }
    }

    if (should_try_vdd &&
        recover_display_with_vdd(launch_session, is_reconfigure, display_result, recovery_result)) {
      set_auto_recovery_status(tree, recovery_result);
      return true;
    }

    if (should_try_vdd) {
      set_auto_recovery_status(tree, recovery_result);
      set_display_config_error(tree, display_result);
      return false;
    }

    if (!video::probe_encoders()) {
      set_auto_recovery_status(tree, recovery_result);
      return true;
    }

    if (recovery_result.attempted) {
      // A failed display-level recovery already tried the strongest display
      // fallback. Keep that result instead of masking it with encoder-only fixes.
      set_auto_recovery_status(tree, recovery_result);
    }
    else if (retry_deferred_display_config(launch_session, is_reconfigure, display_result, recovery_result) ||
        recover_display_with_vdd(launch_session, is_reconfigure, display_result, recovery_result) ||
        recover_with_temporary_encoder_config(recovery_result)) {
      set_auto_recovery_status(tree, recovery_result);
      return true;
    }

    set_auto_recovery_status(tree, recovery_result);

    const bool deferred_display_likely_caused_probe_failure =
      display_result.result == display_device::session_t::configure_result_t::result_e::deferred_retry &&
      video::last_encoder_probe_result.error == video::probe_error_e::no_active_display;

    if (!display_result || deferred_display_likely_caused_probe_failure) {
      set_display_config_error(tree, display_result);
    }
    else {
      set_video_probe_error(tree);
    }

    return false;
  }

}  // namespace nvhttp::stream_start
