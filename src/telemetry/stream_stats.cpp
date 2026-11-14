/**
 * @file src/telemetry/stream_stats.cpp
 * @brief The per-session sums behind stream_quality.
 */
#include "stream_stats.h"

#include <algorithm>
#include <cmath>

using json = nlohmann::json;
using namespace std::literals;

namespace telemetry {

  namespace {
    std::string
    lower(std::string_view text) {
      std::string out { text };
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    bool
    contains(const std::string &haystack, std::string_view needle) {
      return haystack.find(needle) != std::string::npos;
    }
  }  // namespace

  json
  dims_to_props(const stream_dims_t &d) {
    json props {
      { "client_kind", d.client_kind.empty() ? "other" : d.client_kind },
      { "client_os", d.client_os.empty() ? "other" : d.client_os },
      { "codec", d.codec.empty() ? "other" : d.codec },
      { "width", d.width },
      { "height", d.height },
      { "fps", d.fps },
      { "hdr", d.hdr },
      { "audio_codec", d.audio_codec },
      { "transport", d.transport },
      { "app_kind", d.app_kind },
    };
    if (!d.sparxie_game.empty()) props["sparxie_game"] = d.sparxie_game;
    return props;
  }

  std::string
  result_of_stop_reason(std::string_view r) {
    if (r == "control_timeout"sv) return "timeout";
    if (r == "protocol_error"sv || r == "video_ended"sv || r == "audio_ended"sv) return "error";
    if (r == "host_terminate"sv) return "kicked";
    return "normal";  // none, control_disconnect, client_cancel
  }

  std::string
  classify_app_kind(std::string_view name, std::string_view cmd) {
    const auto n = lower(name);
    const auto c = lower(cmd);
    if (contains(c, "sparxie-launch") || contains(c, "sparxie")) return "sparxie";
    if (contains(n, "big picture") || contains(n, "bigpicture") || contains(n, "big screen") || contains(n, "bigscreen") || contains(n, "大屏") || contains(c, "bigpicture") || contains(c, "fullscreen")) return "bigscreen";
    if (contains(c, "steam://") || contains(n, "steam") || contains(c, "steam.exe")) return "steam";
    if (n == "desktop" || n == "桌面" || contains(n, "desktop") || contains(n, "桌面")) return "desktop";
    return "other";
  }

  std::string
  transport_of(bool uses_altp) {
    return uses_altp ? "altp" : "enet";
  }

  sparxie_app_t
  sparxie_app_of_apps(std::string_view apps_json_text, std::string_view app_id) {
    sparxie_app_t out;
    if (app_id.empty()) return out;
    const auto root = json::parse(apps_json_text.begin(), apps_json_text.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("apps") || !root["apps"].is_array()) return out;
    for (const auto &app : root["apps"]) {
      if (!app.is_object() || !app.contains("id")) continue;
      const auto &id = app["id"];
      const bool same = (id.is_string() && id.get<std::string>() == app_id) || (id.is_number_integer() && std::to_string(id.get<long long>()) == app_id);
      if (!same) continue;
      if (!app.contains("sparxie") || !app["sparxie"].is_object()) return out;
      out.is_sparxie = true;
      const auto game = app["sparxie"].find("game");
      if (game != app["sparxie"].end() && game->is_string()) out.game = sparxie_game_id(game->get<std::string>());
      return out;
    }
    return out;
  }

  std::string
  client_kind_of(std::string_view family) {
    // only the families the host itself recognises (src/client_family.h); anything else, and every name a person gave a device, is "other"
    static const char *const known[] = { "moonlight-vplus-android", "moonlight-vplus-desktop", "moonlight-macos-enhanced", "voidlink", "moonlight-upstream", "unknown" };
    const auto id = lower(family);
    for (const char *k : known)
      if (id == k) return id;
    return "other";
  }

  std::string
  sparxie_game_id(std::string_view game) {
    // the games of the Sparxie catalogue (src/sparxie/model.h kGames); a typed or unknown value is "other"
    static const char *const known[] = { "genshin", "starrail", "zzz", "hi3", "wuwa", "pgr", "arknights", "endfield", "nte" };
    const auto id = lower(game);
    for (const char *k : known)
      if (id == k) return id;
    return id.empty() ? std::string {} : std::string { "other" };
  }

  std::string
  codec_of_video_format(int video_format) {
    switch (video_format) {
      case 0: return "h264";
      case 1: return "hevc";
      case 2: return "av1";
      default: return "other";
    }
  }

  std::string
  client_os_of(std::string_view os) {
    const auto s = lower(os);
    if (contains(s, "windows") || contains(s, "win32")) return "windows";
    if (contains(s, "ipados")) return "ipados";
    if (contains(s, "ios") || contains(s, "iphone")) return "ios";
    if (contains(s, "tvos")) return "tvos";
    if (contains(s, "mac") || contains(s, "darwin")) return "macos";
    if (contains(s, "android")) return "android";
    if (contains(s, "linux")) return "linux";
    return "other";
  }

  // ---------------------------------------------------------------------------------------------------------------------

  stream_stats_t::stream_stats_t(std::uint32_t session_id, stream_dims_t dims, std::int64_t start_ms):
      session_id_(session_id), dims_(std::move(dims)), start_ms_(start_ms) {}

  void
  stream_stats_t::on_encoded_frame(std::int64_t now_ms) noexcept {
    if (frames_.fetch_add(1, std::memory_order_relaxed) == 0) first_frame_at_.store(now_ms, std::memory_order_relaxed);
  }

  void
  stream_stats_t::on_encode_ms(double ms) noexcept {
    std::unique_lock lock { mutex_, std::try_to_lock };
    if (lock.owns_lock()) encode_.add(ms);
  }

  void
  stream_stats_t::on_capture_ms(double ms) noexcept {
    std::unique_lock lock { mutex_, std::try_to_lock };
    if (lock.owns_lock()) capture_.add(ms);
  }

  std::optional<std::int64_t>
  stream_stats_t::first_frame_ms() const noexcept {
    const auto at = first_frame_at_.load(std::memory_order_relaxed);
    if (at < 0) return std::nullopt;
    return std::max<std::int64_t>(0, at - start_ms_);
  }

  bool
  stream_stats_t::take_first_frame_report() noexcept {
    if (first_frame_at_.load(std::memory_order_relaxed) < 0) return false;
    bool expected = false;
    return first_frame_reported_.compare_exchange_strong(expected, true);
  }

  void
  stream_stats_t::tick(std::int64_t now_ms, const session_probe_t &probe) {
    const auto frames = frames_.load(std::memory_order_relaxed);
    if (last_tick_ms_ >= 0 && now_ms > last_tick_ms_) {
      const double seconds = static_cast<double>(now_ms - last_tick_ms_) / 1000.0;
      const double fps = static_cast<double>(frames - last_frames_) / seconds;
      // only the time the stream was flowing counts toward the rate: the first frame has not been seen before
      if (frames > 0) {
        fps_.add(fps);
        const bool slow = dims_.fps > 0 && fps < 0.4 * dims_.fps;
        slow_ticks_ = slow ? slow_ticks_ + 1 : 0;
        // a stall: two ticks in a row under 40 % of the target rate; one stall until the rate comes back
        if (slow_ticks_ >= 2 && !in_stall_) {
          in_stall_ = true;
          ++stalls_;
        }
        else if (!slow) {
          in_stall_ = false;
        }
      }
    }
    last_frames_ = frames;
    last_tick_ms_ = now_ms;
    if (probe.rtt_ms && *probe.rtt_ms >= 0) rtt_.add(*probe.rtt_ms);
    if (probe.loss_pct) {
      loss_sum_ += std::clamp(*probe.loss_pct, 0.0, 100.0);
      ++loss_n_;
    }
    if (probe.measured_kbps > 0) {
      kbps_sum_ += probe.measured_kbps;
      ++kbps_n_;
    }
    if (probe.dropped_frames) {
      if (!first_dropped_) first_dropped_ = *probe.dropped_frames;
      last_dropped_ = *probe.dropped_frames;
    }
  }

  json
  stream_stats_t::quality_props(std::int64_t now_ms) const {
    json p = json::object();
    const auto frames = frames_.load(std::memory_order_relaxed);
    if (fps_.count() > 0) {
      p["fps_p50"] = fps_.percentile(50);
      // the low end of the rate: the value 95 % of the samples reached
      p["fps_p5"] = fps_.percentile(5);
    }
    if (last_dropped_ && first_dropped_ && frames > 0) {
      const double dropped = std::max(0, *last_dropped_ - *first_dropped_);
      p["frame_drop_pct"] = std::min(100.0, 100.0 * dropped / static_cast<double>(frames + static_cast<std::uint64_t>(dropped)));
    }
    {
      std::lock_guard lock { mutex_ };
      if (encode_.count() > 0) {
        p["encode_ms_p50"] = encode_.percentile(50);
        p["encode_ms_p95"] = encode_.percentile(95);
      }
      if (capture_.count() > 0) {
        p["capture_ms_p50"] = capture_.percentile(50);
        p["capture_ms_p95"] = capture_.percentile(95);
      }
    }
    if (rtt_.count() > 0) {
      p["network_rtt_ms_p50"] = rtt_.percentile(50);
      p["network_rtt_ms_p95"] = rtt_.percentile(95);
    }
    if (loss_n_ > 0) p["loss_pct"] = loss_sum_ / loss_n_;
    if (kbps_n_ > 0) p["bitrate_kbps_avg"] = static_cast<std::int64_t>(std::llround(kbps_sum_ / kbps_n_));
    p["reconnects"] = reconnects_;
    p["stalls"] = stalls_;
    p["codec"] = dims_.codec.empty() ? "other" : dims_.codec;
    p["transport"] = dims_.transport;
    p["duration_s"] = std::max<std::int64_t>(0, (now_ms - start_ms_) / 1000);
    return p;
  }

}  // namespace telemetry
