/**
 * @file src/crash/crash_context.h
 * @brief What the host was doing when it crashed: streaming or not, codec, resolution, frame rate, transport, client family.
 *
 * Header-only and lock-free: the stream code writes a few integers when a stream starts or ends, the crash handler reads them without allocating (it runs inside a
 * fault). They end up as the "ctx" object of the crash report: fixed words and numbers, no names, no text.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>

namespace crash::context {

  struct state_t {
    std::atomic<int> streaming { -1 };  ///< -1 unknown, 0 idle, 1 streaming
    std::atomic<int> codec { 0 };
    std::atomic<int> width { 0 };
    std::atomic<int> height { 0 };
    std::atomic<int> fps { 0 };
    std::atomic<int> transport { 0 };
    std::atomic<int> client { 0 };
    std::atomic<int> hdr { -1 };
    std::atomic<int> phase { 0 };  ///< the start-up phase the main thread is in (phase_e), 0 once the host is up
  };

  /// What the main thread is doing while the host starts. The words are fixed lowercase ASCII: a hang or a crash during the start names the phase it was in,
  /// so it can be sorted without symbols. The telemetry Worker accepts exactly these words (cleanCrashCtx).
  enum class phase_e : int {
    none = 0,
    display_restore,  ///< recovery of the display state after a crash (display_device)
    nvprefs,  ///< NVIDIA control panel settings
    apps,  ///< apps.json and the app list
    enhancement,  ///< HDR/image enhancement components
    platform_init,  ///< platf::init (capture, audio, input back ends)
    proc_init,  ///< proc::init
    pyrowave,  ///< PyroWave runtime probe
    network,  ///< http::init, client fingerprint rules, mDNS and UPnP
    webhook,  ///< webhook configuration and runtime
    listeners,  ///< the HTTP/RTSP listener threads
    encoder_probe,  ///< the encoder probe (graphics driver calls)
    input_wait,  ///< waiting for the input devices
    tray,  ///< system tray and GUI agent
  };

  inline state_t &
  state() {
    static state_t s;
    return s;
  }

  inline const char *
  phase_word(int v) {
    switch (static_cast<phase_e>(v)) {
      case phase_e::display_restore: return "display-restore";
      case phase_e::nvprefs: return "nvprefs";
      case phase_e::apps: return "apps";
      case phase_e::enhancement: return "enhancement";
      case phase_e::platform_init: return "platform-init";
      case phase_e::proc_init: return "proc-init";
      case phase_e::pyrowave: return "pyrowave";
      case phase_e::network: return "network";
      case phase_e::webhook: return "webhook";
      case phase_e::listeners: return "listeners";
      case phase_e::encoder_probe: return "encoder-probe";
      case phase_e::input_wait: return "input-wait";
      case phase_e::tray: return "tray";
      default: return nullptr;
    }
  }
  inline const char *
  codec_word(int v) {
    switch (v) {
      case 1: return "h264";
      case 2: return "hevc";
      case 3: return "av1";
      case 4: return "other";
      default: return nullptr;
    }
  }
  inline const char *
  transport_word(int v) {
    switch (v) {
      case 1: return "enet";
      case 2: return "altp";
      case 3: return "other";
      default: return nullptr;
    }
  }
  inline const char *
  client_word(int v) {
    switch (v) {
      case 1: return "moonlight-pc";
      case 2: return "moonlight-macos";
      case 3: return "moonlight-ios";
      case 4: return "moonlight-android";
      case 5: return "moonlight-qt";
      case 6: return "foundation";
      case 7: return "web";
      case 8: return "other";
      default: return nullptr;
    }
  }

  inline int
  codec_id(std::string_view s) {
    return s == "h264" ? 1 : s == "hevc" ? 2 : s == "av1" ? 3 : s.empty() ? 0 : 4;
  }
  inline int
  transport_id(std::string_view s) {
    return s == "enet" ? 1 : s == "altp" ? 2 : s.empty() ? 0 : 3;
  }
  /// The client family from the catalogue id the stream statistics already use ("other" when it is not one of the known families).
  inline int
  client_id(std::string_view kind) {
    auto has = [&](std::string_view needle) { return kind.find(needle) != std::string_view::npos; };
    if (kind.empty()) return 0;
    if (has("mac")) return 2;
    if (has("ios") || has("ipad")) return 3;
    if (has("android")) return 4;
    if (has("web") || has("browser")) return 7;
    if (has("qt")) return 5;
    if (has("foundation") || has("alkaid")) return 6;
    if (has("moonlight") || has("pc") || has("windows")) return 1;
    return 8;
  }

  /// A stream has started: what it is.
  inline void
  stream_started(std::string_view codec, int width, int height, int fps, bool hdr, std::string_view transport, std::string_view client_kind) noexcept {
    auto &s = state();
    s.codec = codec_id(codec);
    s.width = width;
    s.height = height;
    s.fps = fps;
    s.hdr = hdr ? 1 : 0;
    s.transport = transport_id(transport);
    s.client = client_id(client_kind);
    s.streaming = 1;
  }
  /// The last stream has ended.
  inline void
  stream_ended() noexcept {
    state().streaming = 0;
  }
  /// The main thread enters a start-up phase; `phase_e::none` when the start is over.
  inline void
  set_phase(phase_e phase) noexcept {
    state().phase.store(static_cast<int>(phase), std::memory_order_relaxed);
  }

}  // namespace crash::context
