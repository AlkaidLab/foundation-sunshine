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
  };

  inline state_t &
  state() {
    static state_t s;
    return s;
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

}  // namespace crash::context
