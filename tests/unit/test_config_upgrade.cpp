/**
 * @file test_config_upgrade.cpp
 * @brief A sunshine.conf of an older release loads into the same fields with the same values.
 *
 * The fixtures under tests/fixtures/config_upgrade were written with the keys and
 * value syntax of the two releases named there. Each row below is one key of the
 * fixture: the getter reads the field the new parser assigns and the expected
 * string is the value the old parser accepted. A key that the new parser no
 * longer reads is reported through doctor::config_problems instead (checked
 * separately), so a silent ignore cannot pass.
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "src/config.h"
#include "src/doctor/doctor.h"
#include "src/platform/common.h"

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  const fs::path fixture_root = fs::path(SUNSHINE_SOURCE_DIR) / "tests" / "fixtures" / "config_upgrade";
  constexpr const char *eras[] { "2026.1031", "2026.1112" };

  std::string
  read_text(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
  }

  template <class T>
  std::string
  join(const T &values) {
    std::string out;
    for (const auto &value : values) {
      if (!out.empty()) out += ',';
      out += std::to_string(value);
    }
    return out;
  }

  std::string
  show(int value) { return std::to_string(value); }
  std::string
  show(std::int64_t value) { return std::to_string(value); }
  std::string
  show(std::uint16_t value) { return std::to_string(value); }
  std::string
  show(bool value) { return value ? "true" : "false"; }
  std::string
  show(const std::string &value) { return value; }
  std::string
  show(const std::vector<std::string> &values) {
    std::string out;
    for (const auto &value : values) {
      if (!out.empty()) out += ',';
      out += value;
    }
    return out;
  }
  std::string
  show(const std::vector<int> &values) { return join(values); }
  std::string
  show(const std::unordered_map<int, int> &values) {
    std::vector<std::pair<int, int>> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (const auto &[key, value] : sorted) {
      if (!out.empty()) out += ',';
      out += std::to_string(key) + ':' + std::to_string(value);
    }
    return out;
  }
  std::string
  show(std::chrono::milliseconds value) { return std::to_string(value.count()); }
  std::string
  show(std::chrono::duration<double> value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value.count());
    return buffer;
  }
  std::string
  show(const std::optional<int> &value) { return value ? std::to_string(*value) : "<unset>"; }
  std::string
  show(const std::optional<bool> &value) { return value ? (*value ? "true" : "false") : "<unset>"; }

  // The path fields are made absolute against appdata; the fixture names them relative to it.
  std::string
  relative_to_appdata(const std::string &path) {
    const auto relative = fs::path(path).lexically_normal().lexically_relative(platf::appdata().lexically_normal());
    return relative.generic_string();
  }

  struct row_t {
    const char *key;
    std::function<std::string()> actual;
    std::string expected;
  };

  std::vector<row_t>
  rows() {
    return {
      { "qp", [] { return show(config::video.qp); }, "31" },
      { "min_threads", [] { return show(config::video.min_threads); }, "4" },
      { "hevc_mode", [] { return show(config::video.hevc_mode); }, "2" },
      { "av1_mode", [] { return show(config::video.av1_mode); }, "1" },
      { "sw_preset", [] { return show(config::video.sw.sw_preset); }, "medium" },
      { "sw_tune", [] { return show(config::video.sw.sw_tune); }, "ull" },
      { "nvenc_preset", [] { return show(config::video.nv.quality_preset); }, "5" },
      { "nvenc_frame_budget_guard", [] { return show(config::video.nv.frame_budget_guard); }, "false" },
      { "nvenc_vbv_increase", [] { return show(config::video.nv.vbv_percentage_increase); }, "10" },
      { "nvenc_spatial_aq", [] { return show(config::video.nv.adaptive_quantization); }, "true" },
      { "nvenc_twopass", [] { return show(static_cast<int>(config::video.nv.two_pass)); }, "2" },
      { "nvenc_h264_cavlc", [] { return show(config::video.nv.h264_cavlc); }, "true" },
      { "nvenc_split_encode", [] { return show(static_cast<int>(config::video.nv.split_frame_encoding)); }, "3" },
      { "nvenc_lookahead_depth", [] { return show(config::video.nv.lookahead_depth); }, "16" },
      { "nvenc_lookahead_level", [] { return show(static_cast<int>(config::video.nv.lookahead_level)); }, "2" },
      { "nvenc_temporal_filter", [] { return show(static_cast<int>(config::video.nv.temporal_filter_level)); }, "4" },
      { "nvenc_rate_control", [] { return show(static_cast<int>(config::video.nv.rate_control_mode)); }, "1" },
      { "nvenc_target_quality", [] { return show(config::video.nv.target_quality); }, "20" },
      { "nvenc_cuda_array_input", [] { return show(config::video.nv.cuda_array_input); }, "true" },
      { "nvenc_realtime_hags", [] { return show(config::video.nv_realtime_hags); }, "false" },
      { "nvenc_manage_driver_settings", [] { return show(config::video.nv_manage_driver_settings); }, "false" },
      { "nvenc_vdd_force_vsync", [] { return show(config::video.nv_vdd_force_vsync); }, "true" },
      { "nvenc_opengl_vulkan_on_dxgi", [] { return show(config::video.nv_opengl_vulkan_on_dxgi); }, "false" },
      { "nvenc_latency_over_power", [] { return show(config::video.nv_sunshine_high_power_mode); }, "false" },
      { "qsv_preset", [] { return show(config::video.qsv.qsv_preset); }, "5" },
      { "qsv_coder", [] { return show(config::video.qsv.qsv_cavlc); }, "0" },
      { "qsv_slow_hevc", [] { return show(config::video.qsv.qsv_slow_hevc); }, "true" },
      { "amd_coder", [] { return show(config::video.amd.amd_coder); }, "1" },
      { "amd_preanalysis", [] { return show(config::video.amd.amd_preanalysis); }, "1" },
      { "amd_vbaq", [] { return show(config::video.amd.amd_vbaq); }, "1" },
      { "amd_enforce_hrd", [] { return show(config::video.amd.amd_enforce_hrd); }, "0" },
      { "amd_qvbr_quality", [] { return show(config::video.amd.amd_qvbr_quality); }, "30" },
      { "amd_ltr_frames", [] { return show(config::video.amd.amd_ltr_frames); }, "2" },
      { "amd_slices_per_frame", [] { return show(config::video.amd.amd_slices_per_frame); }, "2" },
      { "amd_avcodec_compat", [] { return show(config::video.amd.amd_avcodec_compat); }, "true" },
      { "amd_vpe", [] { return show(config::video.amd.amd_vpe); }, "true" },
      { "amd_hevc_gop_size", [] { return show(config::video.amd.amd_hevc_gop_size); }, "120" },
      { "amd_multi_hw_instance", [] { return show(config::video.amd.amd_multi_hw_instance); }, "false" },
      { "amd_high_motion_qb", [] { return show(config::video.amd.amd_high_motion_qb); }, "true" },
      { "amd_lowlatency_mode", [] { return show(config::video.amd.amd_lowlatency_mode); }, "false" },
      { "amd_input_queue_size", [] { return show(config::video.amd.amd_input_queue_size); }, "8" },
      { "amd_av1_latency_mode", [] { return show(config::video.amd.amd_av1_latency_mode); }, "2" },
      { "vt_coder", [] { return show(config::video.vt.vt_coder); }, "1" },
      { "vt_realtime", [] { return show(config::video.vt.vt_realtime); }, "0" },
      { "vaapi_strict_rc_buffer", [] { return show(config::video.vaapi.strict_rc_buffer); }, "true" },
      { "capture", [] { return show(config::video.capture); }, "ddx" },
      { "encoder", [] { return show(config::video.encoder); }, "nvenc" },
      { "adapter_name", [] { return show(config::video.adapter_name); }, "NVIDIA GeForce RTX 4080" },
      { "output_name", [] { return show(config::video.output_name); }, "DISPLAY2" },
      { "capture_target", [] { return show(config::video.capture_target); }, "window" },
      { "window_title", [] { return show(config::video.window_title); }, "Notepad" },
      { "display_device_prep", [] { return show(config::video.display_device_prep); }, "1" },
      { "resolution_change", [] { return show(config::video.resolution_change); }, "2" },
      { "manual_resolution", [] { return show(config::video.manual_resolution); }, "2560x1440" },
      { "refresh_rate_change", [] { return show(config::video.refresh_rate_change); }, "2" },
      { "manual_refresh_rate", [] { return show(config::video.manual_refresh_rate); }, "120" },
      { "hdr_prep", [] { return show(config::video.hdr_prep); }, "1" },
      { "max_bitrate", [] { return show(config::video.max_bitrate); }, "80000" },
      { "variable_refresh_rate", [] { return show(config::video.variable_refresh_rate); }, "true" },
      { "minimum_fps_target", [] { return show(config::video.minimum_fps_target); }, "30" },
      { "input_activity_boost", [] { return show(config::video.input_activity_boost); }, "false" },
      { "input_activity_boost_fps", [] { return show(config::video.input_activity_boost_fps); }, "0" },
      { "input_activity_boost_window_ms", [] { return show(config::video.input_activity_boost_window_ms); }, "0" },
      { "hdr_luminance_analysis", [] { return show(config::video.hdr_luminance_analysis); }, "off" },
      { "wgc_disable_secure_desktop", [] { return show(config::video.wgc_disable_secure_desktop); }, "true" },
      { "dynamic_resolution_follow_display", [] { return show(config::video.dynamic_resolution_follow_display); }, "false" },
      { "vdd_keep_enabled", [] { return show(config::video.vdd_keep_enabled); }, "true" },
      { "vdd_headless_create", [] { return show(config::video.vdd_headless_create_enabled); }, "true" },
      { "vdd_reuse", [] { return show(config::video.vdd_reuse); }, "true" },
      { "vdd_vulkan_hdr_bridge", [] { return show(config::video.vdd_vulkan_hdr_bridge); }, "false" },
      { "capture_cursor", [] { return show(config::video.capture_cursor); }, "false" },
      { "downscaling_quality", [] { return show(config::video.downscaling_quality); }, "fast" },
      { "capture_compute_shader", [] { return show(config::video.capture_compute_shader); }, "on" },
      { "pkey", [] { return relative_to_appdata(config::nvhttp.pkey); }, "credentials/cakey.pem" },
      { "cert", [] { return relative_to_appdata(config::nvhttp.cert); }, "credentials/cacert.pem" },
      { "sunshine_name", [] { return show(config::nvhttp.sunshine_name); }, "Living Room PC" },
      { "clients", [] { return show(config::nvhttp.clients); }, "[{\"uuid\":\"8f14e45f-ceea-467a-9e4a-2b1f6f0a1c11\",\"name\":\"Tablet\"}]" },
      { "log_path", [] { return relative_to_appdata(config::sunshine.log_file); }, "sunshine.log" },
      { "file_state", [] { return relative_to_appdata(config::nvhttp.file_state); }, "sunshine_state.json" },
      { "file_mappings", [] { return show(config::nvhttp.file_mappings); }, "[]" },
      { "usb_forwarding_enabled", [] { return show(config::nvhttp.usb_forwarding_enabled); }, "true" },
      { "credentials_file", [] { return relative_to_appdata(config::sunshine.credentials_file); }, "sunshine_state.json" },
      { "external_ip", [] { return show(config::nvhttp.external_ip); }, "home.example.com" },
      { "resolutions", [] { return show(config::nvhttp.resolutions); }, "1280x720,1920x1080,2560x1440" },
      { "fps", [] { return show(config::nvhttp.fps); }, "60,90.5" },
      { "sleep_mode", [] { return show(config::nvhttp.sleep_mode); }, "2" },
      { "pair_max_attempts", [] { return show(config::nvhttp.pair_max_attempts); }, "5" },
      { "client_fingerprint_remote_rules", [] { return show(config::nvhttp.client_fingerprint_remote_rules); }, "false" },
      { "client_fingerprint_rules_url", [] { return show(config::nvhttp.client_fingerprint_rules_url); }, "https://example.com/rules.json" },
      { "client_fingerprint_rules_certificate", [] { return relative_to_appdata(config::nvhttp.client_fingerprint_rules_certificate); }, "credentials/rules.pem" },
      { "client_fingerprint_rules_refresh_hours", [] { return show(config::nvhttp.client_fingerprint_rules_refresh_hours); }, "48" },
      { "audio_sink", [] { return show(config::audio.sink); }, "Speakers (Realtek)" },
      { "virtual_sink", [] { return show(config::audio.virtual_sink); }, "Virtual Speakers" },
      { "keep_sink_default", [] { return show(config::audio.keep_sink_default); }, "true" },
      { "microphone_redirect_backend", [] { return show(config::audio.microphone_redirect_backend); }, "usbip_experimental" },
      { "stream_audio", [] { return show(config::audio.stream); }, "true" },
      { "stream_mic", [] { return show(config::audio.stream_mic); }, "false" },
      { "install_steam_audio_drivers", [] { return show(config::audio.install_steam_drivers); }, "false" },
      { "mat_live", [] { return show(config::audio.mat_live); }, "true" },
      { "origin_web_ui_allowed", [] { return show(config::nvhttp.origin_web_ui_allowed); }, "wan" },
      { "stop_on_last_video_session", [] { return show(config::stream.stop_on_last_video_session); }, "true" },
      { "lan_encryption_mode", [] { return show(config::stream.lan_encryption_mode); }, "1" },
      { "wan_encryption_mode", [] { return show(config::stream.wan_encryption_mode); }, "2" },
      { "file_apps", [] { return relative_to_appdata(config::stream.file_apps); }, "apps.json" },
      { "fec_percentage", [] { return show(config::stream.fec_percentage); }, "10" },
      { "fec_auto", [] { return show(config::stream.fec_auto); }, "true" },
      { "fec_auto_max_percentage", [] { return show(config::stream.fec_auto_max_percentage); }, "60" },
      { "gamepad", [] { return show(config::input.gamepad); }, "ds5" },
      { "ds4_back_as_touchpad_click", [] { return show(config::input.ds4_back_as_touchpad_click); }, "false" },
      { "motion_as_ds4", [] { return show(config::input.motion_as_ds4); }, "false" },
      { "touchpad_as_ds4", [] { return show(config::input.touchpad_as_ds4); }, "true" },
      { "enable_dsu_server", [] { return show(config::input.enable_dsu_server); }, "true" },
      { "client_gamepad_override", [] { return show(config::input.client_gamepad_override); }, "false" },
      { "mouse", [] { return show(config::input.mouse); }, "true" },
      { "keyboard", [] { return show(config::input.keyboard); }, "true" },
      { "controller", [] { return show(config::input.controller); }, "true" },
      { "always_send_scancodes", [] { return show(config::input.always_send_scancodes); }, "false" },
      { "high_resolution_scrolling", [] { return show(config::input.high_resolution_scrolling); }, "true" },
      { "native_pen_touch", [] { return show(config::input.native_pen_touch); }, "false" },
      { "native_touchpad_optimization", [] { return show(config::input.native_touchpad_optimization); }, "false" },
      { "virtual_mouse", [] { return show(config::input.virtual_mouse); }, "false" },
      { "amf_draw_mouse_cursor", [] { return show(config::input.amf_draw_mouse_cursor); }, "true" },
      { "clipboard_sync", [] { return show(config::input.clipboard_sync); }, "false" },
      { "notify_pre_releases", [] { return show(config::sunshine.notify_pre_releases); }, "true" },
      { "system_tray", [] { return show(config::sunshine.system_tray); }, "false" },
      { "address_family", [] { return show(config::sunshine.address_family); }, "both" },
      { "bind_address", [] { return show(config::sunshine.bind_address); }, "192.0.2.10" },
      { "restore_log", [] { return show(config::sunshine.restore_log); }, "true" },
      { "max_log_size_mb", [] { return show(config::sunshine.max_log_size_mb); }, "10" },
      { "locale", [] { return show(config::sunshine.locale); }, "zh" },
      { "tray_locale", [] { return show(config::sunshine.tray_locale); }, "zh" },
      { "widget_token", [] { return show(config::sunshine.widget_token); }, "compat-token" },
    };
  }

  std::vector<std::string>
  unrecognized_problems() {
    std::vector<std::string> out;
    for (const auto &problem : doctor::config_problems()) {
      if (problem.rfind("Unrecognized configurable option [", 0) == 0) out.push_back(problem);
    }
    return out;
  }

  class ConfigUpgrade: public testing::Test {
  protected:
    static void
    SetUpTestSuite() {
      // the struct defaults as the new build starts them, before any fixture is parsed
      default_mic_backend = config::audio.microphone_redirect_backend;
      default_launch_splash = config::stream.launch_splash;
      default_host_audio = config::audio.host_audio;
      default_video_pacing_threshold = config::video.video_pacing_threshold;
      default_session_idle = config::sunshine.session_idle_minutes;
      default_session_absolute = config::sunshine.session_absolute_hours;
      default_session_remember_idle = config::sunshine.session_remember_idle_minutes;
      default_session_remember_days = config::sunshine.session_remember_days;
    }

    int
    parse_era(const std::string &era) {
      // The flag keys flip bits and the list keys append, so a second fixture
      // starts from the state a first start of that release would have.
      config::sunshine.flags.reset();
      config::sunshine.prep_cmds.clear();
      config::input.keybindings.clear();
      const auto base = platf::appdata();
      const auto config_path = base / ("config-upgrade-" + era + ".conf");
      fs::copy_file(fixture_root / era / "sunshine.conf", config_path, fs::copy_options::overwrite_existing);
      fs::copy_file(fixture_root / era / "apps.json", base / "apps.json", fs::copy_options::overwrite_existing);
      fs::copy_file(fixture_root / era / "sunshine_state.json", base / "sunshine_state.json", fs::copy_options::overwrite_existing);
      std::vector<std::string> arguments { "sunshine-config-upgrade-test", config_path.string() };
      std::vector<char *> argv;
      for (auto &argument : arguments) argv.push_back(argument.data());
      argv.push_back(nullptr);
      return config::parse(static_cast<int>(argv.size()) - 1, argv.data());
    }

    static std::string default_mic_backend;
    static bool default_launch_splash;
    static bool default_host_audio;
    static int default_video_pacing_threshold;
    static int default_session_idle;
    static int default_session_absolute;
    static int default_session_remember_idle;
    static int default_session_remember_days;
  };

  std::string ConfigUpgrade::default_mic_backend;
  bool ConfigUpgrade::default_launch_splash = false;
  bool ConfigUpgrade::default_host_audio = false;
  int ConfigUpgrade::default_video_pacing_threshold = 0;
  int ConfigUpgrade::default_session_idle = 0;
  int ConfigUpgrade::default_session_absolute = 0;
  int ConfigUpgrade::default_session_remember_idle = 0;
  int ConfigUpgrade::default_session_remember_days = 0;
}  // namespace

TEST_F(ConfigUpgrade, EveryOldKeyLandsInItsField) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0) << "the new parser rejected " << era;
    const auto parsed = config::parse_config(read_text(fixture_root / era / "sunshine.conf"));
    for (const auto &row : rows()) {
      if (!parsed.contains(row.key)) continue;
#ifndef _WIN32
      // window capture does not exist on this platform: the two keys are
      // Windows-only and the old Linux host ignored them as well.
      if (std::string_view(row.key) == "capture_target" || std::string_view(row.key) == "window_title") continue;
#endif
      EXPECT_EQ(row.actual(), row.expected) << row.key << " in " << era;
    }
  }
}

TEST_F(ConfigUpgrade, OnlyTheKnownDroppedKeyIsReportedUnknown) {
  ASSERT_EQ(parse_era("2026.1112"), 0);
  ASSERT_EQ(parse_era("2026.1031"), 0);
  const auto problems = unrecognized_problems();
  const auto reported = [&](const std::string &key) {
    const auto text = "Unrecognized configurable option [" + key + "]";
    return std::find(problems.begin(), problems.end(), text) != problems.end();
  };

  // Every key the 2026.1112 parser knew is still read, except the two that only
  // exist in the Windows capture path.
  for (const auto &[key, _] : config::parse_config(read_text(fixture_root / "2026.1112" / "sunshine.conf"))) {
    if (key == "capture_target" || key == "window_title") continue;
    EXPECT_FALSE(reported(key)) << key << " is no longer read";
  }
  // 2026.1031 has one more key, which no later release reads.
  for (const auto &[key, _] : config::parse_config(read_text(fixture_root / "2026.1031" / "sunshine.conf"))) {
    if (key == "capture_target" || key == "window_title" || key == "vdd_borrowed_texture") continue;
    EXPECT_FALSE(reported(key)) << key << " is no longer read";
  }
  EXPECT_TRUE(reported("vdd_borrowed_texture"));
#ifdef _WIN32
  EXPECT_FALSE(reported("capture_target"));
  EXPECT_FALSE(reported("window_title"));
#else
  // Windows-only settings: the old Linux host ignored them with the same warning.
  EXPECT_TRUE(reported("capture_target"));
  EXPECT_TRUE(reported("window_title"));
#endif
}

TEST_F(ConfigUpgrade, PortsAndTimers) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0);
    EXPECT_EQ(config::sunshine.port, 48000);
    EXPECT_EQ(config::nvhttp.file_mapping_port, 48021);
    EXPECT_EQ(config::nvhttp.usb_forwarding_port, 48022);
    EXPECT_EQ(config::input.dsu_server_port, 26761);
    EXPECT_EQ(config::stream.ping_timeout, 5000ms);
    EXPECT_EQ(config::input.back_button_timeout, 300ms);
    EXPECT_EQ(config::input.key_repeat_delay, 250ms);
    EXPECT_NEAR(config::input.key_repeat_period.count(), 1.0 / 30.0, 1e-9);
    EXPECT_EQ(config::sunshine.min_log_level, 1);  // debug
  }
}

TEST_F(ConfigUpgrade, FlagsAndKeybindingsKeepTheirMeaning) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0);
    // flags = p flips UPNP, upnp = enabled flips it again: the old meaning of both keys together
    EXPECT_FALSE(config::sunshine.flags[config::flag::UPNP]);
    EXPECT_TRUE(config::sunshine.flags[config::flag::CLOSE_VERIFY_SAFE]);
    EXPECT_FALSE(config::sunshine.flags[config::flag::MDNS_BROADCAST]);
    EXPECT_EQ(config::input.keybindings.at(0x10), 0xA0);
    EXPECT_EQ(config::input.keybindings.at(0x11), 0xA2);
    EXPECT_EQ(config::input.keybindings.at(0x12), 0xA4);
    EXPECT_EQ(config::input.keybindings.at(0xA5), 0x5B);  // key_rightalt_to_key_win = enabled
  }
}

TEST_F(ConfigUpgrade, GlobalPrepCommandIsOneEntry) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0);
    ASSERT_EQ(config::sunshine.prep_cmds.size(), 1u);
    EXPECT_EQ(config::sunshine.prep_cmds.front().do_cmd, "echo do");
    EXPECT_EQ(config::sunshine.prep_cmds.front().undo_cmd, "echo undo");
    EXPECT_FALSE(config::sunshine.prep_cmds.front().elevated);
  }
}

TEST_F(ConfigUpgrade, AmdAggregateKeysSetEveryCodec) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0);
    // Each codec names its own value of the same word; the numbers are the AMF
    // SDK's, identical in the reference headers and the non-Windows fallback.
    EXPECT_EQ(config::video.amd.amd_quality_h264, 2);
    EXPECT_EQ(config::video.amd.amd_quality_hevc, 0);
    EXPECT_EQ(config::video.amd.amd_quality_av1, 30);
    EXPECT_EQ(config::video.amd.amd_rc_h264, 3);
    EXPECT_EQ(config::video.amd.amd_rc_hevc, 1);
    EXPECT_EQ(config::video.amd.amd_rc_av1, 1);
    EXPECT_EQ(config::video.amd.amd_usage_h264, 2);
    EXPECT_EQ(config::video.amd.amd_usage_hevc, 2);
    EXPECT_EQ(config::video.amd.amd_usage_av1, 1);
  }
}

TEST_F(ConfigUpgrade, VtSoftwareSetsBothFields) {
  for (const char *era : eras) {
    SCOPED_TRACE(era);
    ASSERT_EQ(parse_era(era), 0);
    EXPECT_EQ(config::video.vt.vt_allow_sw, 1);
    EXPECT_EQ(config::video.vt.vt_require_sw, 1);
  }
}

TEST_F(ConfigUpgrade, KeysAddedSinceTheOldReleasesTakeTheirDefaults) {
  ASSERT_EQ(parse_era("2026.1112"), 0);
  EXPECT_EQ(config::audio.host_audio, default_host_audio);
  EXPECT_EQ(config::stream.launch_splash, default_launch_splash);
  EXPECT_EQ(config::video.video_pacing_threshold, default_video_pacing_threshold);
  EXPECT_EQ(config::sunshine.session_idle_minutes, default_session_idle);
  EXPECT_EQ(config::sunshine.session_absolute_hours, default_session_absolute);
  EXPECT_EQ(config::sunshine.session_remember_idle_minutes, default_session_remember_idle);
  EXPECT_EQ(config::sunshine.session_remember_days, default_session_remember_days);
  EXPECT_TRUE(config::nvhttp.external_endpoints.empty());
  EXPECT_TRUE(config::nvhttp.stun_servers.empty());
  EXPECT_TRUE(config::nvhttp.fs_host_icon.empty());
  // the documented new defaults, independent of the copy captured at start-up
  EXPECT_FALSE(default_host_audio);
  EXPECT_TRUE(default_launch_splash);
  EXPECT_EQ(default_video_pacing_threshold, 64 * 1024);
  EXPECT_EQ(default_session_idle, 30);
  EXPECT_EQ(default_session_absolute, 12);
  EXPECT_EQ(default_session_remember_idle, 7 * 24 * 60);
  EXPECT_EQ(default_session_remember_days, 30);
}

TEST_F(ConfigUpgrade, MicrophoneBackendDefaultChangedButEveryOldValueIsStillAccepted) {
  // 2026.1112 defaulted to vb_cable; the new build defaults to auto.
  EXPECT_EQ(default_mic_backend, "auto");
  for (const std::string value : { "auto", "vb_cable", "usbip_experimental", "disabled" }) {
    const auto base = platf::appdata();
    const auto config_path = base / ("config-upgrade-mic-" + value + ".conf");
    std::ofstream(config_path, std::ios::binary | std::ios::trunc) << "microphone_redirect_backend = " << value << "\n";
    std::vector<std::string> arguments { "sunshine-config-upgrade-test", config_path.string() };
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ASSERT_EQ(config::parse(static_cast<int>(argv.size()) - 1, argv.data()), 0) << value;
    EXPECT_EQ(config::audio.microphone_redirect_backend, value);
  }
}

TEST_F(ConfigUpgrade, AnEmptyValueIsOmittedNotReadAsASpace) {
  const auto parsed = config::parse_config("bind_address =   \nclient_gamepad_override =\n");
  EXPECT_FALSE(parsed.contains("bind_address"));
  EXPECT_FALSE(parsed.contains("client_gamepad_override"));
}

TEST_F(ConfigUpgrade, AnUnknownKeySurvivesAFullSettingsSaveWhenTheUiSendsItBack) {
  const auto path = platf::appdata() / "config-upgrade-unknown.conf";
  std::ofstream(path, std::ios::binary | std::ios::trunc) << "sunshine_name = old\nfuture_key = kept\n";
  const auto saved_path = config::sunshine.config_file;
  config::sunshine.config_file = path.string();
  struct restore_t {
    std::string path;
    ~restore_t() { config::sunshine.config_file = path; }
  } restore { saved_path };

  // The Web UI receives every key of the file and posts the whole table back.
  const auto current = config::parse_config(read_text(path));
  std::map<std::string, std::string> full(current.begin(), current.end());
  full["sunshine_name"] = "new";
  ASSERT_TRUE(config::update_full_config(full));
  const auto after_full = config::parse_config(read_text(path));
  EXPECT_EQ(after_full.at("sunshine_name"), "new");
  EXPECT_EQ(after_full.at("future_key"), "kept");

  // A partial save (the Panel's one-key writers) merges into the file.
  ASSERT_TRUE(config::update_config({ { "sunshine_name", "newer" } }));
  const auto after_partial = config::parse_config(read_text(path));
  EXPECT_EQ(after_partial.at("sunshine_name"), "newer");
  EXPECT_EQ(after_partial.at("future_key"), "kept");
}
