/**
 * @file src/telemetry/stream_stats.h
 * @brief What one streaming session adds up for the stream_quality event: frame rate, encode and capture time, round trip, loss, bitrate, stalls.
 *
 * Pure code. The per-frame calls are cheap and never wait (a contended lock drops the sample instead of blocking the encoder thread); the one-per-interval
 * tick() reads the session's own counters and the client's reports.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "events.h"

namespace telemetry {

  /** The dimensions of a session (stream_start / stream_end). Every text is an allow-listed value or a catalogue id, never a name a person chose. */
  struct stream_dims_t {
    std::string client_kind;  ///< client family id (catalogue id), "other" when not known
    std::string client_os;
    std::string codec;  ///< h264 | hevc | av1 | pyrowave | other
    int width = 0;
    int height = 0;
    int fps = 0;
    bool hdr = false;
    std::string audio_codec = "opus";
    std::string transport = "enet";
    std::string app_kind = "other";
    std::string sparxie_game;
  };

  /** What the host knows about a session at one tick, read from its own state and the client's reports. */
  struct session_probe_t {
    std::uint32_t session_id = 0;
    std::optional<double> rtt_ms;
    std::optional<double> loss_pct;
    std::optional<int> dropped_frames;  ///< cumulative, as the client reports it
    int measured_kbps = 0;  ///< 0 when the window is empty
  };

  nlohmann::json dims_to_props(const stream_dims_t &dims);

  /** stop_reason_name() of the host -> the contract's result: normal | error | timeout | kicked. */
  std::string result_of_stop_reason(std::string_view stop_reason);

  /** Classify an application from what the host knows about it; only the answer (desktop|bigscreen|sparxie|steam|other) leaves the computer, never the name. */
  std::string classify_app_kind(std::string_view name, std::string_view cmd);

  /**
   * The transport of a stream: the one place that decides. "altp" when the session runs over ALTP, else "enet". On this branch line the host has no ALTP session flag yet, so the callers pass false;
   * when the flag exists, only the caller changes.
   */
  std::string transport_of(bool uses_altp);

  /** What apps.json says about an application's Sparxie block. */
  struct sparxie_app_t {
    bool is_sparxie = false;  ///< the entry has a `sparxie` object (a Sparxie application, however it is started)
    std::string game;  ///< its `game` as a catalogue id (genshin, starrail, zzz, hi3, endfield, ...); empty when absent or not a catalogue id, never free text
  };
  /** Look up the application whose `id` is `app_id` in the text of apps.json. Not found, no block, unreadable text: not a Sparxie application. */
  sparxie_app_t sparxie_app_of_apps(std::string_view apps_json_text, std::string_view app_id);

  /** A client family name -> a catalogue id ("moonlight-macos" stays, anything else is cleaned or becomes "other"). */
  std::string client_kind_of(std::string_view family);
  /** A Sparxie game id from the catalogue (genshin, starrail, zzz, hi3, wuwa, pgr, arknights, endfield, nte); another value is "other"; empty stays empty. */
  std::string sparxie_game_id(std::string_view game);
  /** The wire video format (0 H.264, 1 HEVC, 2 AV1, 3+ experimental) -> codec value. */
  std::string codec_of_video_format(int video_format);
  /** Normalise a client OS string ("macOS 15.2", "windows", "ios"...) to the allow-listed enum. */
  std::string client_os_of(std::string_view os);

  class stream_stats_t {
  public:
    stream_stats_t(std::uint32_t session_id, stream_dims_t dims, std::int64_t start_ms);

    // per-frame, from the encoder and capture threads: never blocks
    void on_encoded_frame(std::int64_t now_ms) noexcept;
    void on_encode_ms(double ms) noexcept;
    void on_capture_ms(double ms) noexcept;

    /** Once per interval (the sender thread). */
    void tick(std::int64_t now_ms, const session_probe_t &probe);

    /** Milliseconds from the start of the session to its first encoded frame; empty while none was seen. */
    std::optional<std::int64_t> first_frame_ms() const noexcept;
    /** The first-frame time was read (so the startup event is sent once). */
    bool take_first_frame_report() noexcept;

    /** The reconnect count: sessions of the same client that began within 30 s of the previous one ending. Set by the service. */
    void set_reconnects(int reconnects) noexcept { reconnects_ = reconnects; }

    std::uint32_t session_id() const noexcept { return session_id_; }
    const stream_dims_t &dims() const noexcept { return dims_; }
    std::int64_t start_ms() const noexcept { return start_ms_; }

    /** The props of the stream_quality event for the session so far. */
    nlohmann::json quality_props(std::int64_t now_ms) const;

  private:
    std::uint32_t session_id_;
    stream_dims_t dims_;
    std::int64_t start_ms_;

    std::atomic<std::uint64_t> frames_ { 0 };
    std::atomic<std::int64_t> first_frame_at_ { -1 };
    std::atomic<bool> first_frame_reported_ { false };

    mutable std::mutex mutex_;  // histograms of the hot path; taken with try_lock there
    histogram_t encode_ { 500.0, 1000 };
    histogram_t capture_ { 500.0, 1000 };

    // sampler state (sender thread only)
    histogram_t fps_ { 1000.0, 2000 };
    histogram_t rtt_ { 2000.0, 2000 };
    std::uint64_t last_frames_ = 0;
    std::int64_t last_tick_ms_ = -1;
    int slow_ticks_ = 0;
    bool in_stall_ = false;
    int stalls_ = 0;
    double loss_sum_ = 0;
    int loss_n_ = 0;
    double kbps_sum_ = 0;
    int kbps_n_ = 0;
    std::optional<int> first_dropped_;
    std::optional<int> last_dropped_;
    int reconnects_ = 0;
  };

}  // namespace telemetry
