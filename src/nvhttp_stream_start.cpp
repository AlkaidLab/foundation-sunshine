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
#include "display_device/vdd_capability.h"
#include "hdr/session_target.h"
#include "logging.h"
#include "touch_keyboard_session.h"
#include "video.h"

#include <algorithm>

namespace nvhttp::stream_start {

  namespace pt = boost::property_tree;

  namespace {

    struct auto_recovery_result_t {
      bool attempted { false };
      bool succeeded { false };
      std::string action;
      std::string detail;
    };

    video::probe_target_t
    make_probe_target(const display_device::display_intent_t &intent) {
      if (intent.target == display_device::display_intent_t::target_e::physical &&
          intent.device_id.empty()) {
        return { {}, video::probe_target_policy_e::backend_autoselect };
      }

      return {
        intent.device_id,
        intent.target == display_device::display_intent_t::target_e::vdd ?
          video::probe_target_policy_e::vdd_compatible :
          video::probe_target_policy_e::exact
      };
    }

    video::probe_target_t
    make_vdd_probe_target() {
      return { config::video.output_name, video::probe_target_policy_e::vdd_compatible };
    }

    video::probe_target_t
    make_configured_probe_target(
      const display_device::display_intent_t &intent,
      const video::probe_target_t &original_target) {
      return intent.target == display_device::display_intent_t::target_e::vdd ?
               make_vdd_probe_target() :
               original_target;
    }

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
      rtsp_stream::launch_session_t &launch_session,
      bool is_reconfigure,
      display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result,
      const video::probe_target_t &probe_target,
      bool &probe_matches_display_state,
      bool refresh_vdd_probe_target = false) {
      using result_e = display_device::session_t::configure_result_t::result_e;
      if (display_result.result != result_e::deferred_retry) {
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
        probe_matches_display_state = false;
        display_result = display_device::session_t::get().configure_display(config::video, launch_session, is_reconfigure);

        if (display_result.result == result_e::deferred_retry) {
          continue;
        }
        if (display_result.result != result_e::success) {
          recovery_result.detail = "Display configuration failed during a deferred retry.";
          return false;
        }

        hdr::adopt_vdd_calibration_if_needed(launch_session);

        const auto target = refresh_vdd_probe_target ? make_vdd_probe_target() : probe_target;
        const auto probe_result = video::probe_encoders(target);
        probe_matches_display_state = true;
        if (!probe_result) {
          recovery_result.succeeded = true;
          recovery_result.detail = "Display configuration became available after a short retry.";
          BOOST_LOG(info) << "Recovered stream startup after deferred display configuration retry";
          return true;
        }

        recovery_result.detail = "Display configuration became available, but encoder probing still failed.";
        return false;
      }

      recovery_result.detail = "Display configuration was still unavailable after short retries.";
      return false;
    }

    enum class configure_outcome_e {
      ok,
      retry_later,
      mode_refused,
      topology_refused,
      current_only,
      fatal
    };

    configure_outcome_e
    classify_configure_result(display_device::session_t::configure_result_t::result_e result) {
      using result_e = display_device::session_t::configure_result_t::result_e;

      switch (result) {
        case result_e::success:
          return configure_outcome_e::ok;
        case result_e::deferred_retry:
          return configure_outcome_e::retry_later;
        case result_e::modes_fail:
        case result_e::hdr_states_fail:
          return configure_outcome_e::mode_refused;
        case result_e::topology_fail:
        case result_e::primary_display_fail:
          return configure_outcome_e::topology_refused;
        case result_e::file_save_fail:
        case result_e::revert_fail:
          return configure_outcome_e::current_only;
        case result_e::parse_fail:
        case result_e::vdd_not_installed:
        case result_e::vdd_unavailable:
        case result_e::vdd_create_failed:
        default:
          return configure_outcome_e::fatal;
      }
    }

    /**
     * @brief Whether a virtual display may be brought in for something the user did not ask for.
     *
     * Hosts without a working driver must never see a VDD error for an automatic
     * decision, so the fallback is skipped before display configuration starts.
     */
    bool
    vdd_fallback_allowed(const display_device::display_intent_t &intent) {
      if (intent.target == display_device::display_intent_t::target_e::vdd) {
        BOOST_LOG(debug) << "Skipping automatic VDD fallback: the requested VDD is what just failed";
        return false;
      }

      if (intent.device_prep == display_device::parsed_config_t::device_prep_e::no_operation) {
        BOOST_LOG(info) << "Skipping automatic VDD fallback: display preparation is no_operation";
        return false;
      }

      const auto state = display_device::vdd_capability::query_state();
      if (state != display_device::vdd_capability::state_e::ready) {
        BOOST_LOG(info) << "Skipping automatic VDD fallback, driver state: "
                        << display_device::vdd_capability::to_string(state);
        return false;
      }

      return true;
    }

    bool
    validate_display_intent(pt::ptree &tree, const display_device::display_intent_t &intent) {
      if (intent.target == display_device::display_intent_t::target_e::unavailable) {
        set_sunshine_error(
          tree,
          503,
          "The display selected by the client is not connected.",
          "DISPLAY_NOT_CONNECTED",
          "Connect the selected display, or choose another display in the client.",
          "select_available_display",
          "display",
          "display_preflight",
          true);
        return false;
      }

      if (intent.target != display_device::display_intent_t::target_e::vdd) {
        return true;
      }

      const auto state = display_device::vdd_capability::query_state();
      using state_e = display_device::vdd_capability::state_e;
      switch (state) {
        case state_e::ready:
          return true;
        case state_e::unsupported_platform:
          set_sunshine_error(
            tree,
            501,
            "Virtual display streaming is not supported on this host.",
            "VDD_NOT_SUPPORTED",
            "Use a physical display or install a Sunshine build with VDD support.",
            "use_physical_display",
            "display",
            "vdd_preflight",
            false);
          return false;
        case state_e::driver_missing:
          set_sunshine_error(
            tree,
            503,
            "The virtual display driver is not installed or not running.",
            "VDD_DRIVER_MISSING",
            "Install or repair ZakoVDD, then try again.",
            "repair_vdd_driver",
            "display",
            "vdd_preflight",
            true);
          return false;
        case state_e::driver_unreachable:
          set_sunshine_error(
            tree,
            503,
            "The virtual display driver is currently unreachable.",
            "VDD_DRIVER_UNREACHABLE",
            "Restart or repair ZakoVDD, then try again.",
            "repair_vdd_driver",
            "display",
            "vdd_preflight",
            true);
          return false;
      }

      return false;
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
      recovery_session.synthetic_hdr = launch_session.synthetic_hdr;
      recovery_session.frame_pipeline_policy = launch_session.frame_pipeline_policy;
      recovery_session.frame_pipeline_policy_resolved = launch_session.frame_pipeline_policy_resolved;
      recovery_session.enable_sops = launch_session.enable_sops;
      recovery_session.enable_mic = launch_session.enable_mic;
      recovery_session.use_vdd = true;
      recovery_session.custom_screen_mode = launch_session.custom_screen_mode;
      recovery_session.hdr_capabilities = launch_session.hdr_capabilities;
      recovery_session.reported_hdr_capabilities = launch_session.reported_hdr_capabilities;
      recovery_session.hdr_target_source = launch_session.hdr_target_source;
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
    commit_vdd_recovery_to_launch_session(rtsp_stream::launch_session_t &launch_session,
      const rtsp_stream::launch_session_t &recovery_session) {
      launch_session.use_vdd = true;
      launch_session.hdr_capabilities = recovery_session.hdr_capabilities;
      launch_session.hdr_target_source = recovery_session.hdr_target_source;
      launch_session.env["SUNSHINE_CLIENT_USE_VDD"] = "true";
      launch_session.env["SUNSHINE_CLIENT_DISPLAY_NAME"] = config::video.output_name;
      launch_session.sync_hdr_environment();
    }

    /**
     * @brief Probe whatever the displays are showing right now, without configuring anything.
     */
    bool
    try_current_display(
      const display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result,
      const video::probe_target_t &probe_target) {
      recovery_result = {
        true,
        false,
        "current_display_settings_fallback",
        "Display configuration failed; probing the current display state before using fallback recovery."
      };

      BOOST_LOG(warning) << "Display configuration failed; continuing with current display settings if encoder probing succeeds";
      if (!video::probe_encoders(probe_target)) {
        recovery_result.succeeded = true;
        recovery_result.detail = "Encoder probing succeeded with the current display settings.";
        return true;
      }

      recovery_result.detail = "The current display settings were kept, but encoder probing still failed.";
      if (display_result.cleanup_on_failure) {
        display_device::session_t::get().restore_state();
      }
      return false;
    }

    /**
     * @brief Bring up a VDD-backed display and stream that instead.
     * @note On failure the VDD switch is undone and @p display_result is restored,
     *       so a later fallback still sees the real reason the launch failed.
     */
    bool
    try_vdd_display(
      rtsp_stream::launch_session_t &launch_session,
      bool is_reconfigure,
      display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result) {
      recovery_result = {
        true,
        false,
        "vdd_display_recovery",
        "The requested display could not carry this stream; trying a VDD-backed display."
      };

      const auto original_display_result = display_result;
      const auto original_probe_result = video::last_encoder_probe_result;
      rtsp_stream::launch_session_t recovery_session {};
      fill_vdd_recovery_session(recovery_session, launch_session);

      display_result = display_device::session_t::get().configure_display(config::video, recovery_session, is_reconfigure);
      if (display_result.result == display_device::session_t::configure_result_t::result_e::deferred_retry) {
        auto deferred_recovery_result = auto_recovery_result_t {};
        bool probe_matches_display_state = false;
        if (retry_deferred_display_config(
              recovery_session,
              is_reconfigure,
              display_result,
              deferred_recovery_result,
              make_vdd_probe_target(),
              probe_matches_display_state,
              true)) {
          commit_vdd_recovery_to_launch_session(launch_session, recovery_session);
          recovery_result.succeeded = true;
          recovery_result.detail = "A VDD-backed display became available after a short retry and encoder probing succeeded.";
          BOOST_LOG(info) << "Recovered stream startup by preparing a VDD-backed display after a deferred retry";
          return true;
        }

        // A deferred attempt owns a retry timer and may already have created a
        // VDD, so undo it before trying physical-display recovery.
        display_device::session_t::get().restore_state();
        display_result = original_display_result;
        video::last_encoder_probe_result = original_probe_result;
        recovery_result.detail = "VDD-backed display recovery remained deferred and was rolled back.";
        return false;
      }

      if (display_result.result != display_device::session_t::configure_result_t::result_e::success) {
        // configure_display reverts its own partial work except for mode/HDR
        // failures, which it hands to the caller. This attempt was ours, so there
        // is no caller to hand it to - drop the half-configured VDD here.
        if (display_result.cleanup_on_failure) {
          display_device::session_t::get().restore_state();
        }

        // Keep the failure the caller has to report: the VDD attempt was ours, not
        // the user's, so its error code would hide why the launch actually failed.
        display_result = original_display_result;
        video::last_encoder_probe_result = original_probe_result;
        recovery_result.detail = "VDD-backed display recovery was attempted, but the VDD display configuration failed.";
        return false;
      }
      hdr::adopt_vdd_calibration_if_needed(recovery_session);

      if (!video::probe_encoders(make_vdd_probe_target())) {
        commit_vdd_recovery_to_launch_session(launch_session, recovery_session);
        recovery_result.succeeded = true;
        recovery_result.detail = "A VDD-backed display became available and encoder probing succeeded.";
        BOOST_LOG(info) << "Recovered stream startup by preparing a VDD-backed display";
        return true;
      }

      display_result = original_display_result;
      // The VDD is live at this point but useless, and a later fallback may still
      // want the physical display, so undo the switch we made.
      display_device::session_t::get().restore_state();
      video::last_encoder_probe_result = original_probe_result;
      recovery_result.detail = "VDD-backed display recovery was attempted, but encoder probing still failed.";
      return false;
    }

    /**
     * @brief Try only the display fallbacks that can address the observed configuration failure.
     */
    bool
    recover_display(
      const display_device::display_intent_t &intent,
      configure_outcome_e outcome,
      bool no_display_to_capture,
      rtsp_stream::launch_session_t &launch_session,
      bool is_reconfigure,
      display_device::session_t::configure_result_t &display_result,
      auto_recovery_result_t &recovery_result,
      bool &display_recovery_attempted) {
      const auto try_current = [&] {
        display_recovery_attempted = true;
        return try_current_display(
          display_result,
          recovery_result,
          make_configured_probe_target(intent, make_probe_target(intent)));
      };
      const auto try_vdd = [&] {
        if (!vdd_fallback_allowed(intent)) {
          return false;
        }
        display_recovery_attempted = true;
        return try_vdd_display(launch_session, is_reconfigure, display_result, recovery_result);
      };

      if (no_display_to_capture) {
        return try_vdd();
      }

      switch (outcome) {
        case configure_outcome_e::mode_refused:
          if (intent.user_named_display || intent.target == display_device::display_intent_t::target_e::vdd) {
            return try_current();
          }
          return try_vdd() || try_current();
        case configure_outcome_e::topology_refused:
          return try_current() || try_vdd();
        case configure_outcome_e::current_only:
          return try_current();
        case configure_outcome_e::ok:
        case configure_outcome_e::retry_later:
        case configure_outcome_e::fatal:
        default:
          return false;
      }
    }

    bool
    recover_with_temporary_encoder_config(
      auto_recovery_result_t &recovery_result,
      const video::probe_target_t &probe_target) {
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
        if (!video::probe_encoders(probe_target)) {
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
    const auto intent = display_device::resolve_display_intent(config::video, launch_session);
    if (!validate_display_intent(tree, intent)) {
      return false;
    }

    // Display configuration can change the active capture target, so probe
    // encoders only after the display stack has settled.
    auto display_result = display_device::session_t::get().configure_display(config::video, launch_session, is_reconfigure);
    auto outcome = classify_configure_result(display_result.result);
    if (display_result) {
      hdr::adopt_vdd_calibration_if_needed(launch_session);
    }

#ifdef _WIN32
    // Touch-keyboard experience: when the client (or its server profile)
    // opts in, enable the AutoInvoke registry key group for the session.
    // The touch digitizer itself is expected to be present already (VDD /
    // remote-software virtual digitizer); failures degrade to a log line.
    {
      bool touch_effective = false;
      if (launch_session.touch_keyboard < 0) {
        touch_effective = config::get_client_touch_keyboard_enabled(launch_session.client_cert_uuid);
      }
      else {
        touch_effective = launch_session.touch_keyboard == 1;
      }
      touch_kb::start_session(touch_effective);
    }
#endif
    auto_recovery_result_t recovery_result;
    const auto probe_target = make_probe_target(intent);
    bool probe_matches_display_state = false;

    if (outcome == configure_outcome_e::fatal) {
      set_display_config_error(tree, display_result);
      return false;
    }

    // Track whether the configured display was actually passed to the encoder
    // probe. Deferred configurations are reclassified after their final retry.
    const bool initial_state_can_be_probed =
      outcome == configure_outcome_e::ok || outcome == configure_outcome_e::retry_later;
    if (initial_state_can_be_probed) {
      const auto configured_probe_target = make_configured_probe_target(intent, probe_target);
      const auto probe_result = video::probe_encoders(configured_probe_target);
      probe_matches_display_state = true;
      if (!probe_result) {
        return true;
      }

      if (retry_deferred_display_config(
            launch_session,
            is_reconfigure,
            display_result,
            recovery_result,
            probe_target,
            probe_matches_display_state,
            intent.target == display_device::display_intent_t::target_e::vdd)) {
        set_auto_recovery_status(tree, recovery_result);
        return true;
      }

      // Every retry reconfigures the display, so display_result now describes the
      // last attempt instead of the deferral. Re-classify, or the fallback below and
      // the error we report would still be answering the original question.
      outcome = classify_configure_result(display_result.result);
      if (outcome == configure_outcome_e::fatal) {
        set_auto_recovery_status(tree, recovery_result);
        set_display_config_error(tree, display_result);
        return false;
      }
    }

    const bool no_display_to_capture =
      probe_matches_display_state &&
      video::last_encoder_probe_result.error == video::probe_error_e::no_active_display;
    bool display_recovery_attempted = false;

    if (recover_display(
          intent,
          outcome,
          no_display_to_capture,
          launch_session,
          is_reconfigure,
          display_result,
          recovery_result,
          display_recovery_attempted)) {
      set_auto_recovery_status(tree, recovery_result);
      return true;
    }

    auto_recovery_result_t encoder_recovery_result;
    if (!display_recovery_attempted && probe_matches_display_state &&
        recover_with_temporary_encoder_config(
          encoder_recovery_result,
          make_configured_probe_target(intent, probe_target))) {
      set_auto_recovery_status(tree, encoder_recovery_result);
      return true;
    }
    if (!display_recovery_attempted && encoder_recovery_result.attempted) {
      recovery_result = std::move(encoder_recovery_result);
    }

    set_auto_recovery_status(tree, recovery_result);

    const bool display_is_the_cause =
      outcome == configure_outcome_e::mode_refused ||
      outcome == configure_outcome_e::topology_refused ||
      outcome == configure_outcome_e::current_only ||
      outcome == configure_outcome_e::retry_later;

    if (display_is_the_cause) {
      set_display_config_error(tree, display_result);
    }
    else {
      set_video_probe_error(tree);
    }

    return false;
  }

}  // namespace nvhttp::stream_start
