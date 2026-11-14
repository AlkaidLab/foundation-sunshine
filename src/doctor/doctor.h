/**
 * @file src/doctor/doctor.h
 * @brief One-click host health check ("体检"): checks grouped by area, each with a status and a fix.
 * @details Everything here is read-only. The evaluators turn facts into items and are pure, so the
 *          unit tests drive them with made-up facts; the collectors in doctor_checks.cpp gather the facts.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace doctor {
  constexpr int JSON_VERSION = 1;

  enum class status_e {
    pass,
    warn,
    fail,
    skip,  ///< 未检查: the host cannot know, or checking would change system state
  };

  enum class lang_e {
    en,
    zh,
  };

  /** Something the UI can do for a fix. */
  struct action_t {
    std::string kind;  ///< "open" (target is page:<id> or setting:<key>), "rerun" (target is a group), "restart" (target "sunshine"), "link" (a URL),
                       ///< "repair" (target driver:<id> or service:<what>; the control panel runs it, the host only asks for it)
    std::string target;
    bool confirm = false;  ///< the UI asks before it acts
    int level = 0;  ///< 1 leads somewhere, 2 acts reversibly (restart), 3 needs administrator rights (repair); 0 = from the kind
    std::string effect;  ///< what it will do, shown before it runs (level 2 and 3)
  };

  struct fix_t {
    std::string text;  ///< empty for a pass
    std::optional<action_t> action;
  };

  struct item_t {
    std::string id;  ///< stable, "<group>.<check>[.<n>]"
    std::string group;
    status_e status = status_e::skip;
    std::string title;
    std::string detail;
    fix_t fix;
  };

  struct group_result_t {
    std::string id;
    std::string title;
    status_e status = status_e::skip;  ///< the worst item; skip only when every item is skipped
    std::int64_t elapsed_ms = 0;
    bool timed_out = false;
    std::vector<item_t> items;
  };

  struct report_t {
    int version = JSON_VERSION;
    bool streaming = false;  ///< a stream starts or runs: a fix that disconnects warns
    std::string checked_at;
    std::int64_t elapsed_ms = 0;
    std::vector<group_result_t> groups;
  };

  /** The groups in display order. */
  const std::vector<std::string_view> &
  group_ids();

  bool
  is_group(std::string_view id);

  /** The groups named by `only` ("encoder,network"), each once; names that are no group go to `unknown`. */
  std::vector<std::string>
  parse_groups(std::string_view value, std::vector<std::string> &unknown);

  std::string
  group_title(std::string_view id, lang_e lang);

  std::string_view
  to_string(status_e status);

  lang_e
  parse_lang(std::string_view value);

  /** fail > warn > pass > skip; an empty list is skip. */
  status_e
  worst(const std::vector<item_t> &items);

  /** Masks addresses (IPv4/IPv6 by reachability::mask_address), MAC addresses and the user part of home paths. */
  std::string
  redact(std::string_view text);

  /** Chinese punctuation next to Chinese text: ",;:" beside a Han character or full-width mark, "?" and ")" after one, "(" beside one, become full width. */
  std::string
  zh_punctuation(std::string_view text);

  /** The level of a fix's action from its kind when the evaluator left it at 0, and the restart's effect; the runner applies it. */
  void
  complete_action(item_t &item, lang_e lang);

  /** What repairing a driver does (the control panel runs it): shown before it runs. */
  std::string
  driver_effect(std::string_view id, lang_e lang);

  /** Applies redact() to every text of an item. */
  void
  redact_item(item_t &item);

  nlohmann::json
  to_json(const item_t &item);

  nlohmann::json
  to_json(const report_t &report);

  // ---- runner ----

  struct check_t {
    std::string group;
    std::chrono::milliseconds timeout { 5000 };
    std::function<std::vector<item_t>(lang_e)> run;
  };

  /**
   * @brief Runs the checks of the requested groups in parallel, each on its own thread with its own timeout.
   * @details A check that overruns is reported as a warn item and left to finish on its thread; its result
   *          is dropped. A check that throws is reported as a fail item with the exception text.
   * @param only empty for every group.
   */
  report_t
  run(const std::vector<check_t> &checks, const std::vector<std::string> &only, lang_e lang);

  // ---- facts and evaluators (pure) ----

  struct gpu_fact_t {
    std::string name;
    std::string vendor;  ///< "nvidia", "amd", "intel", other or empty
    std::string driver_version;  ///< as the platform reports it (Windows registry "32.0.15.6094", Linux "570.86.16")
    bool is_streaming_adapter = false;
    bool integrated = false;
  };

  struct gpu_facts_t {
    bool windows = false;
    std::string configured_adapter;  ///< config adapter_name
    std::vector<gpu_fact_t> gpus;
  };

  /** NVIDIA public driver version ("560.94") from the platform's string; nothing when it cannot be read. */
  std::optional<std::string>
  nvidia_driver_version(std::string_view platform_version, bool windows);

  /** Compares dotted versions numerically; -1, 0 or 1. */
  int
  compare_versions(std::string_view a, std::string_view b);

  struct nvenc_tier_t {
    std::string_view sdk;
    std::string_view windows_min;
    std::string_view linux_min;
    std::string_view adds;  ///< what this SDK tier brings, for the detail line
  };

  /** NVENC SDK tiers this host loads, oldest first (third-party/nvenc-headers/<tier>/README). */
  const std::vector<nvenc_tier_t> &
  nvenc_tiers();

  std::vector<item_t>
  evaluate_gpu(const gpu_facts_t &facts, lang_e lang);

  struct codec_fact_t {
    bool tested = false;
    bool passed = false;
    bool hdr = false;
    bool yuv444 = false;
  };

  struct encoder_fact_t {
    std::string name;  ///< "nvenc", "quicksync", "amdvce", "vaapi", "vulkan", "software"
    bool ran = false;  ///< false: skipped, see skipped_reason
    std::string skipped_reason;  ///< "streaming", "busy", "no_display", "timeout"
    bool usable = false;
    std::string failure;  ///< "display", "unsupported_gpu", "h264_session", ""
    bool configured = false;  ///< config encoder names it
    bool active = false;  ///< the encoder the host chose at its last probe
    codec_fact_t h264, hevc, av1;
    std::int64_t elapsed_ms = 0;
  };

  struct encoder_facts_t {
    std::string configured;  ///< config encoder, empty for automatic
    std::string active;  ///< video::active_encoder_name()
    std::string startup_error;  ///< last_encoder_probe_result message when the host's own probe failed
    std::string startup_hint;
    int hevc_mode = 0;  ///< config hevc_mode (0 auto, 1 off, 2+ on)
    int av1_mode = 0;
    std::vector<encoder_fact_t> encoders;
  };

  std::vector<item_t>
  evaluate_encoders(const encoder_facts_t &facts, lang_e lang);

  struct capture_fact_t {
    bool ran = false;
    std::string skipped_reason;  ///< "streaming", "busy", "no_display"
    bool ok = false;
    std::string method;  ///< config capture, or "auto"
    std::string display;  ///< the display that was captured
    int width = 0;
    int height = 0;
    std::int64_t first_frame_ms = 0;
    std::string failure;  ///< "open", "error", "no_frame"
  };

  std::vector<item_t>
  evaluate_capture(const capture_fact_t &facts, lang_e lang);

  struct vdd_fact_t {
    bool supported_platform = false;
    bool installed = false;
    bool running = false;
    bool control_available = false;
    bool monitor_active = false;
    bool problem_code_valid = false;
    std::uint32_t problem_code = 0;
    std::string driver_version;  ///< empty when not read
    std::optional<int> os_build;  ///< Windows build number
    std::string capture;  ///< config capture
  };

  std::vector<item_t>
  evaluate_vdd(const vdd_fact_t &facts, lang_e lang);

  struct audio_fact_t {
    bool checked = false;  ///< false: the audio control could not be created
    std::string error;
    bool stream_audio = true;  ///< config stream_audio
    std::string sink;  ///< config audio_sink, empty for the default
    bool sink_found = false;
    std::string virtual_sink;  ///< config virtual_sink
    bool virtual_checked = false;
    bool virtual_found = false;
    std::string virtual_name;
    bool mic_checked = false;
    bool mic_supported = false;
    std::string mic_backend;
    std::string mic_state;  ///< mic_redirect_status_t::state
    std::string mic_error;
    bool mic_component = false;
  };

  std::vector<item_t>
  evaluate_audio(const audio_fact_t &facts, lang_e lang);

  struct input_fact_t {
    bool windows = false;
    std::string gamepad;  ///< config gamepad
    bool vigem_checked = false;
    bool vigem_ok = false;
    std::string vigem_error;
    bool uinput_checked = false;
    bool uinput_ok = false;
    std::string uinput_error;
    bool uhid_checked = false;
    bool uhid_ok = false;
    std::string uhid_error;
    bool usbip_supported = false;
    bool usbip_tool_found = false;
    bool usbip_driver_checked = false;
    bool usbip_driver_found = false;
    std::string usbip_driver;
  };

  std::vector<item_t>
  evaluate_input(const input_fact_t &facts, lang_e lang);

  struct port_fact_t {
    std::string proto;
    std::uint16_t port = 0;
    std::string role;
    bool listening = false;
    bool on_demand = false;
    std::string firewall;  ///< "allowed", "blocked", "unknown"
  };

  struct network_fact_t {
    bool checked = false;
    std::string error;
    std::vector<port_fact_t> ports;
    bool upnp_enabled = false;
    std::string router_protocol;  ///< "upnp-igd", "nat-pmp", "pcp", "none"
    std::string wan_ip_class;  ///< "public", "cgnat", "private", "unknown" (reachability::to_string(ip_class_e))
    int mappings_present = 0;
    int mappings_expected = 0;
    std::string public_verdict;  ///< "verified", "likely", "unknown", "unreachable"
    std::vector<std::string> public_reasons;
    bool public_address_unset = false;  ///< no external_ip and no external_endpoints: the public path has no configured address
    std::string mesh_verdict;  ///< "verified", "likely", "unknown", "none"
    bool lan_listening = false;  ///< a stream port is bound on a LAN address
    std::string lan_address;  ///< the address, or "*" for every address
    std::vector<std::string> evidence_kinds;  ///< kinds of the reachability evidence: client-connected-from-*, external-probe-v4|v6 (audit #5)
    std::vector<std::string> ipv6_global;  ///< global IPv6 addresses of the adapters (masked)
    bool ipv6_stability_known = false;  ///< the adapters say which addresses are temporary (audit #4; not reported yet)
    std::string nat_mapping;  ///< STUN mapping behaviour: "endpoint_independent", "dependent", "" when not measured (audit #4)
    std::string pcp;  ///< gateway PCP/NAT-PMP: "pcp", "nat-pmp", "none", "" when not probed (audit #4)
  };

  std::vector<item_t>
  evaluate_network(const network_fact_t &facts, lang_e lang);

  struct disk_fact_t {
    std::string role;  ///< "logs", "config"
    std::string path;
    bool checked = false;
    std::uint64_t available = 0;
    std::uint64_t capacity = 0;
  };

  struct system_fact_t {
    bool windows = false;
    std::vector<disk_fact_t> disks;
    bool time_checked = false;
    bool time_synced = false;
    std::string time_source;
    std::string time_detail;
    bool account_checked = false;
    std::string account;  ///< "system", "admin", "user", "root"
    bool config_writable = true;
    bool reboot_checked = false;
    std::vector<std::string> reboot_reasons;
    std::string os_name;
    std::string os_version;
    std::optional<int> os_build;
  };

  std::vector<item_t>
  evaluate_system(const system_fact_t &facts, lang_e lang);

  /** The build identity (audit #1: /api/version, sunshine --version --json); filled once the build work provides it. */
  struct build_info_t {
    std::string version;
    std::string commit;
    std::string branch;
    bool dirty = false;
    std::string channel;  ///< stable, beta, dev, local
    std::string build_time_utc;
    std::string variant;
    std::string package;
    std::string builder;
  };

  /** The running build's identity; nothing until the build work's build_info lands (the version string is read instead). */
  std::optional<build_info_t>
  current_build_info();

  struct host_fact_t {
    std::optional<build_info_t> build;
    std::string version;
    std::string branch;  ///< the build's branch, "" when unknown
    std::int64_t uptime_s = 0;
    bool crashes_checked = false;
    std::string crash_source;
    std::vector<std::string> crashes;  ///< recent crash records, newest first
    std::vector<std::string> config_problems;  ///< startup config warnings
    int fatal_lines = 0;  ///< Fatal lines in the current log
    bool log_checked = false;
  };

  std::vector<item_t>
  evaluate_host(const host_fact_t &facts, lang_e lang);

  /** Release channel from a version/branch: "stable", "beta", "dev". */
  std::string_view
  channel_of(std::string_view version, std::string_view branch);

  // ---- startup config problems (recorded by config.cpp) ----

  void
  record_config_problem(std::string text);

  std::vector<std::string>
  config_problems();
}  // namespace doctor
