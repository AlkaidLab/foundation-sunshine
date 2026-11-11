/**
 * @file src/doctor/doctor_checks.cpp
 * @brief The collectors of the health check: gather the facts of each group from the running host.
 * @details Nothing here changes the system. Probes that need the GPU (encoders, capture) take the probe lock
 *          and step aside while a stream starts or runs.
 */
#include "doctor_checks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

#include "doctor_os.h"
#include "src/audio.h"
#include "src/config.h"
#include "src/confighttp/network.h"
#include "src/display_device/display_device.h"
#include "src/file_handler.h"
#include "src/globals.h"
#include "src/network_reachability.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/system_info.h"
#include "src/video.h"
#include "version.h"

#ifdef _WIN32
  #include <ViGEm/Client.h>

  #include "src/display_device/vdd_utils.h"
  #include "src/platform/windows/mic_write.h"
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace doctor {
  namespace {
    // A probe that does not return (a driver call, a bus connect) must not hold the group.
    // The thread is abandoned; the caller records a timeout for that probe alone.
    template <class F>
    auto
    finish_or_drop(std::chrono::milliseconds budget, F &&fn, bool &timed_out) -> std::optional<std::invoke_result_t<F>> {
      using result_t = std::invoke_result_t<F>;
      struct slot_t {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::optional<result_t> value;
      };
      auto slot = std::make_shared<slot_t>();
      std::thread([slot, run = std::forward<F>(fn)]() mutable {
        auto value = run();
        std::lock_guard lock { slot->mutex };
        slot->value = std::move(value);
        slot->done = true;
        slot->cv.notify_all();
      }).detach();
      std::unique_lock lock { slot->mutex };
      if (!slot->cv.wait_for(lock, budget, [&] { return slot->done; })) {
        timed_out = true;
        return std::nullopt;
      }
      timed_out = false;
      return std::move(slot->value);
    }

    // Taken during static initialisation, which is the start of the process.
    const auto process_started = std::chrono::steady_clock::now();

    constexpr std::int64_t CRASH_WINDOW_S = 7 * 24 * 3600;

    std::string
    tr(lang_e lang, std::string_view en, std::string_view zh) {
      return std::string { lang == lang_e::zh ? zh : en };
    }

    std::string
    age_text(std::int64_t seconds, lang_e lang) {
      if (seconds < 3600) return std::to_string(std::max<std::int64_t>(1, seconds / 60)) + tr(lang, " min ago", " 分钟前");
      if (seconds < 86400) return std::to_string(seconds / 3600) + tr(lang, " h ago", " 小时前");
      return std::to_string(seconds / 86400) + tr(lang, " d ago", " 天前");
    }

    bool
    contains_icase(std::string_view text, std::string_view part) {
      return std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      }) != text.end();
    }

    bool
    streaming() {
      return rtsp_stream::session_starting_or_active();
    }

    // ---- 显卡与驱动 ----

    std::vector<item_t>
    check_gpu(lang_e lang) {
      const auto snapshot = system_info::sample(config::video.adapter_name, true);
      gpu_facts_t facts;
#ifdef _WIN32
      facts.windows = true;
#endif
      facts.configured_adapter = config::video.adapter_name;
      for (const auto &gpu : snapshot.gpus) {
        // The software rasteriser is listed as an adapter but never encodes.
        if (contains_icase(gpu.name, "Basic Render") || contains_icase(gpu.name, "Basic Display")) continue;
        gpu_fact_t fact;
        fact.name = gpu.name;
        fact.vendor = gpu.vendor;
        fact.driver_version = gpu.driver_version;
        fact.is_streaming_adapter = gpu.is_streaming_adapter;
        // Intel graphics without Arc and AMD with little dedicated memory are the integrated parts.
        fact.integrated = (gpu.vendor == "intel" && !contains_icase(gpu.name, "Arc")) ||
                          (gpu.vendor == "amd" && gpu.vram_mb && *gpu.vram_mb < 1024);
        facts.gpus.push_back(std::move(fact));
      }
      return evaluate_gpu(facts, lang);
    }

    // ---- 编码器 ----

    std::vector<item_t>
    check_encoders(lang_e lang) {
      encoder_facts_t facts;
      facts.configured = config::video.encoder;
      facts.active = video::active_encoder_name();
      facts.hevc_mode = config::video.hevc_mode;
      facts.av1_mode = config::video.av1_mode;
      const bool busy_streaming = streaming();
      if (!busy_streaming && video::last_encoder_probe_result.error != video::probe_error_e::none) {
        facts.startup_error = video::last_encoder_probe_result.message;
        facts.startup_hint = video::last_encoder_probe_result.hint;
      }
      std::chrono::milliseconds group_room_left = 80s;
      for (const auto &name : video::encoder_backend_names()) {
        encoder_fact_t fact;
        fact.name = name;
        fact.configured = !facts.configured.empty() && facts.configured == name;
        fact.active = facts.active == name;
        // Checked before every backend: a stream that starts mid-run gets the GPU back after one backend.
        if (busy_streaming || streaming()) {
          fact.skipped_reason = "streaming";
          facts.encoders.push_back(std::move(fact));
          continue;
        }
        constexpr auto one_backend = 15s;
        // One backend that sits in the driver must not eat the whole group. 15 s is its own budget;
        // 80 s of room stays inside the group's 90 s runner budget.
        if (group_room_left <= 0ms) {
          fact.skipped_reason = "timeout";
          facts.encoders.push_back(std::move(fact));
          continue;
        }
        const auto slice = group_room_left < one_backend ? group_room_left : one_backend;
        const auto began = std::chrono::steady_clock::now();
        bool timed_out = false;
        auto probe = finish_or_drop(slice, [name] { return video::doctor_probe_encoder(name); }, timed_out);
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
        group_room_left -= spent < group_room_left ? spent : group_room_left;
        if (timed_out || !probe) {
          fact.skipped_reason = "timeout";
          facts.encoders.push_back(std::move(fact));
          continue;
        }
        fact.ran = probe->ran;
        fact.skipped_reason = probe->skipped_reason;
        fact.usable = probe->usable;
        fact.failure = probe->failure;
        fact.elapsed_ms = probe->elapsed_ms;
        auto codec = [](const video::doctor_codec_result_t &c) { return codec_fact_t { c.tested, c.passed, c.hdr, c.yuv444 }; };
        fact.h264 = codec(probe->h264);
        fact.hevc = codec(probe->hevc);
        fact.av1 = codec(probe->av1);
        facts.encoders.push_back(std::move(fact));
      }
      return evaluate_encoders(facts, lang);
    }

    // ---- 画面采集 ----

    std::vector<item_t>
    check_capture(lang_e lang) {
      capture_fact_t facts;
      facts.method = config::video.capture.empty() ? "auto"s : config::video.capture;
      if (streaming()) {
        facts.skipped_reason = "streaming";
        return evaluate_capture(facts, lang);
      }
      const auto probe = video::doctor_probe_capture(3000ms);
      facts.ran = probe.ran;
      facts.skipped_reason = probe.skipped_reason;
      facts.ok = probe.ok;
      if (!probe.method.empty()) facts.method = probe.method;
      facts.display = probe.display;
      facts.width = probe.width;
      facts.height = probe.height;
      facts.first_frame_ms = probe.first_frame_ms;
      facts.failure = probe.failure;
      return evaluate_capture(facts, lang);
    }

    // ---- 虚拟显示器 ----

    std::vector<item_t>
    check_vdd(lang_e lang) {
      vdd_fact_t facts;
      facts.capture = config::video.capture;
#ifdef _WIN32
      facts.supported_platform = true;
      const auto status = display_device::vdd_utils::get_vdd_status();
      facts.installed = status.installed;
      facts.running = status.running;
      facts.control_available = status.control_available;
      facts.monitor_active = status.monitor_active;
      facts.problem_code_valid = status.problem_code_valid;
      facts.problem_code = status.problem_code;
      if (facts.installed) facts.driver_version = os::vdd_driver_version();
      facts.os_build = os::windows_build();
#endif
      return evaluate_vdd(facts, lang);
    }

    // ---- 音频 ----

    std::vector<item_t>
    check_audio(lang_e lang) {
      audio_fact_t facts;
      facts.stream_audio = config::audio.stream;
      facts.sink = config::audio.sink;
      facts.virtual_sink = config::audio.virtual_sink;
#ifdef _WIN32
      // The endpoints are enumerated directly: platf::audio_control() would install the Steam speakers when they are missing.
      const auto endpoints = os::audio_endpoints();
      facts.checked = endpoints.checked;
      facts.error = endpoints.error;
      if (endpoints.checked) {
        facts.sink_found = facts.sink.empty() ? std::ranges::any_of(endpoints.render, [](const auto &e) { return e.is_default; }) :
                                                std::ranges::any_of(endpoints.render, [&](const auto &e) { return os::endpoint_matches(e, facts.sink); });
        if (facts.sink.empty()) {
          for (const auto &e : endpoints.render) {
            if (e.is_default) facts.sink = e.name;
          }
        }
        facts.virtual_checked = true;
        for (const auto &e : endpoints.render) {
          const bool match = facts.virtual_sink.empty() ? contains_icase(e.name, "Steam Streaming Speakers") : os::endpoint_matches(e, facts.virtual_sink);
          if (match) {
            facts.virtual_found = true;
            facts.virtual_name = e.name;
          }
        }
      }
      facts.mic_checked = true;
      if (config::audio.stream_mic) {
        const auto mic = platf::audio::mic_redirect_status();
        facts.mic_backend = mic.active_backend.empty() ? mic.configured_backend : mic.active_backend;
        facts.mic_state = mic.state;
        facts.mic_error = mic.error_code.empty() ? mic.fallback_reason : mic.error_code;
        facts.mic_component = mic.component_available;
      }
      else {
        facts.mic_state = "disabled";
      }
#else
      // Linux: the PulseAudio/PipeWire control loads null sinks when it is asked for sink info, so only a named sink is checked.
      if (!facts.sink.empty()) {
        if (auto ref = audio::try_get_audio_ctx_ref(); ref && ref->control) {
          facts.checked = true;
          facts.sink_found = ref->control->is_sink_available(facts.sink);
        }
        else if (auto control = platf::audio_control()) {
          facts.checked = true;
          facts.sink_found = control->is_sink_available(facts.sink);
        }
        else {
          facts.error = "audio control";
        }
      }
      else {
        facts.checked = true;
        facts.sink_found = true;
        facts.sink = tr(lang, "Sunshine's own null sink, created when a stream starts", "Sunshine 自己的虚拟输出(开始串流时创建)");
      }
#endif
      return evaluate_audio(facts, lang);
    }

    // ---- 输入与手柄 ----

    std::vector<item_t>
    check_input(lang_e lang) {
      input_fact_t facts;
      facts.gamepad = config::input.gamepad;
#ifdef _WIN32
      facts.windows = true;
      if (facts.gamepad != "disabled") {
        // The same probe the input layer runs at start: connect a client to the bus, then disconnect.
        // vigem_alloc / vigem_connect have no deadline of their own; 3 s each, then this row fails on its own.
        facts.vigem_checked = true;
        bool alloc_timed_out = false;
        auto client = finish_or_drop(3s, [] { return vigem_alloc(); }, alloc_timed_out);
        if (alloc_timed_out || !client) {
          facts.vigem_error = "no answer within 3 s";
        }
        else if (*client) {
          bool connect_timed_out = false;
          const auto raw = *client;
          auto status = finish_or_drop(3s, [raw] { return vigem_connect(raw); }, connect_timed_out);
          if (connect_timed_out || !status) {
            facts.vigem_error = "no answer within 3 s";
          }
          else {
            facts.vigem_ok = VIGEM_SUCCESS(*status);
            if (facts.vigem_ok)
              vigem_disconnect(raw);
            else
              facts.vigem_error = *status == VIGEM_ERROR_BUS_NOT_FOUND ? "bus not found" : "error " + std::to_string(static_cast<unsigned>(*status));
          }
          vigem_free(raw);
        }
        else {
          facts.vigem_error = "alloc";
        }
      }
#else
      const auto uinput = os::writable("/dev/uinput");
      facts.uinput_checked = uinput.checked;
      facts.uinput_ok = uinput.ok;
      facts.uinput_error = uinput.error;
      const auto uhid = os::writable("/dev/uhid");
      facts.uhid_checked = uhid.checked;
      facts.uhid_ok = uhid.ok;
      facts.uhid_error = uhid.error;
#endif
      const auto usb = os::usbip();
      facts.usbip_supported = usb.supported;
      facts.usbip_tool_found = usb.tool_found;
      facts.usbip_driver_checked = usb.driver_checked;
      facts.usbip_driver_found = usb.driver_found;
      facts.usbip_driver = usb.driver;
      return evaluate_input(facts, lang);
    }

    // ---- 网络 ----

    std::vector<item_t>
    check_network(lang_e lang) {
      network_fact_t facts;
      facts.upnp_enabled = config::sunshine.flags[config::flag::UPNP];
      reachability::snapshot_t snapshot;
      try {
        snapshot = reachability::probe(confighttp::network::configured_probe(), true);
      }
      catch (const std::exception &error) {
        facts.error = error.what();
        return evaluate_network(facts, lang);
      }
      facts.checked = true;
      for (const auto &port : snapshot.ports) {
        facts.ports.push_back({ port.proto, port.port, port.role, port.listening, reachability::binds_on_demand(port), std::string { reachability::to_string(port.firewall) } });
        for (const auto &bound : port.bound) {
          if (facts.lan_listening) break;
          if (bound == "0.0.0.0" || bound == "::" || bound == "*") {
            facts.lan_listening = true;
            facts.lan_address = "*";
          }
          else if (reachability::scope_of(bound) == reachability::scope_e::lan) {
            facts.lan_listening = true;
            facts.lan_address = bound;
          }
        }
      }
      const auto &router = snapshot.public_reachability.router;
      facts.router_protocol = router.protocol;
      facts.wan_ip_class = std::string { reachability::to_string(router.wan_ip_class) };
      facts.mappings_expected = static_cast<int>(router.mappings.size());
      facts.mappings_present = static_cast<int>(std::ranges::count_if(router.mappings, [](const auto &m) { return m.present; }));
      facts.public_verdict = std::string { reachability::to_string(snapshot.public_reachability.verdict) };
      facts.public_reasons = snapshot.public_reachability.reasons;
      facts.public_address_unset =
        snapshot.public_reachability.configured.kind == public_address::kind_e::empty &&
        snapshot.public_reachability.endpoints.empty();
      facts.mesh_verdict = std::string { reachability::to_string(snapshot.mesh.verdict) };
      for (const auto &e : snapshot.public_reachability.evidence) facts.evidence_kinds.push_back(e.kind);
      for (const auto &e : snapshot.mesh.evidence) facts.evidence_kinds.push_back(e.kind);
      // global unicast IPv6 (2000::/3) of the adapters that are up
      for (const auto &nic : system_info::sample(config::video.adapter_name, false).nics) {
        if (!nic.up) continue;
        for (const auto &address : nic.ipv6) {
          if (!address.empty() && (address[0] == '2' || address[0] == '3')) facts.ipv6_global.push_back(reachability::mask_address(address));
        }
      }
      return evaluate_network(facts, lang);
    }

    // ---- 系统 ----

    std::vector<item_t>
    check_system(lang_e lang) {
      system_fact_t facts;
#ifdef _WIN32
      facts.windows = true;
#endif
      auto disk = [](std::string role, const fs::path &path) {
        disk_fact_t fact;
        fact.role = std::move(role);
        fact.path = path.string();
        std::error_code ec;
        const auto space = fs::space(path, ec);
        if (!ec) {
          fact.checked = true;
          fact.available = space.available;
          fact.capacity = space.capacity;
        }
        return fact;
      };
      const auto log_path = file_handler::path_from_utf8(config::sunshine.log_file);
      facts.disks.push_back(disk("logs", log_path.has_parent_path() ? log_path.parent_path() : platf::appdata()));
      facts.disks.push_back(disk("config", platf::appdata()));

      const auto time = os::time_sync();
      facts.time_checked = time.checked;
      facts.time_synced = time.synced;
      facts.time_source = time.source;
      facts.time_detail = time.detail;

      auto account = os::account_kind();
#ifdef _WIN32
      if (is_running_as_system_user) account = "system";
#endif
      facts.account_checked = !account.empty();
      facts.account = account;
#ifndef _WIN32
      facts.config_writable = os::writable(platf::appdata().string()).ok;
#endif

      const auto reboot = os::pending_reboot();
      facts.reboot_checked = reboot.checked;
      for (const auto &reason : reboot.reasons) {
        if (reason == "cbs")
          facts.reboot_reasons.push_back(tr(lang, "Windows component update", "Windows 组件更新"));
        else if (reason == "wu")
          facts.reboot_reasons.push_back(tr(lang, "Windows Update", "Windows 更新"));
        else if (reason == "file_rename")
          facts.reboot_reasons.push_back(tr(lang, "files waiting to be replaced", "等待替换的文件"));
        else if (reason == "packages")
          facts.reboot_reasons.push_back(tr(lang, "updated packages", "已更新的软件包"));
        else
          facts.reboot_reasons.push_back(reason);
      }

      const auto info = system_info::sample(config::video.adapter_name, false);
      facts.os_name = info.os.name;
      facts.os_version = info.os.version;
      facts.os_build = os::windows_build();
      return evaluate_system(facts, lang);
    }

    // ---- 主机状态 ----

    /** Reads the current log: Fatal lines and startup config warnings. */
    void
    scan_log(host_fact_t &facts) {
      std::ifstream file { file_handler::path_from_utf8(config::sunshine.log_file), std::ios::binary };
      if (!file) return;
      facts.log_checked = true;
      std::string line;
      std::size_t read = 0;
      constexpr std::size_t LIMIT = 64ull * 1024 * 1024;
      while (read < LIMIT && std::getline(file, line)) {
        read += line.size() + 1;
        if (line.find("]: Fatal: "sv) != std::string::npos) ++facts.fatal_lines;
        for (const auto marker : { "]: Warning: config: "sv, "]: Error: config: "sv }) {
          const auto at = line.find(marker);
          if (at == std::string::npos) continue;
          auto text = line.substr(at + marker.size());
          while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
          if (std::ranges::find(facts.config_problems, text) == facts.config_problems.end() && facts.config_problems.size() < 32) facts.config_problems.push_back(text);
        }
      }
    }

    std::vector<item_t>
    check_host(lang_e lang) {
      host_fact_t facts;
      facts.build = current_build_info();
      facts.version = PROJECT_VER;
      facts.uptime_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - process_started).count();
      const auto crashes = os::recent_crashes(CRASH_WINDOW_S);
      facts.crashes_checked = crashes.checked;
      facts.crash_source = crashes.source;
      for (const auto age : crashes.ages_s) facts.crashes.push_back(age_text(age, lang));
      facts.config_problems = config_problems();
      scan_log(facts);
      return evaluate_host(facts, lang);
    }
  }  // namespace

  std::vector<check_t>
  host_checks() {
    // The encoder group probes each backend in turn (a few seconds each, longer for software encoding).
    return {
      { "gpu", 8000ms, check_gpu },
      { "encoder", 90000ms, check_encoders },
      { "capture", 15000ms, check_capture },
      { "vdd", 8000ms, check_vdd },
      { "audio", 8000ms, check_audio },
      { "input", 8000ms, check_input },
      { "network", 30000ms, check_network },
      { "system", 8000ms, check_system },
      { "host", 8000ms, check_host },
    };
  }
}  // namespace doctor
