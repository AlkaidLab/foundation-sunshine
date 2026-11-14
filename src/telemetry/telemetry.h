/**
 * @file src/telemetry/telemetry.h
 * @brief Telemetry v1 in the host: one sender for the host's own events and the events of the Panel and the Web UI.
 *
 * What is counted, where it goes and what is never sent is documented for people in docs/privacy_telemetry.md; the wire contract is
 * docs/privacy_telemetry.md. All calls are no-ops until start() ran and while the matching consent is off, never block a streaming
 * thread and never throw.
 *
 * Reuse by the crash reporter (feat/telemetry-crash): install_id(), errors_enabled(), endpoint(), post_to() and the config key names below.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "events.h"
#include "stream_stats.h"

namespace telemetry {

  /** The config keys of the two choices (the same ones the Web UI and the Panel save): usage statistics, error reports. Both on unless the person said off. */
  inline constexpr std::string_view CONFIG_KEY_USAGE = "telemetry_consent";
  inline constexpr std::string_view CONFIG_KEY_ERRORS = "telemetry_errors";
  /** Overrides the endpoint (tests point it at a local mock); default https://api.alkaidlab.com. */
  inline constexpr std::string_view CONFIG_KEY_ENDPOINT = "telemetry_endpoint";
  inline constexpr std::string_view DEFAULT_ENDPOINT = "https://api.alkaidlab.com";

  /** Read the consent, open <config dir>/telemetry, start the sender thread and the log tally. Safe to call once; later calls do nothing. */
  void start();
  /** app_exit, flush to disk (no network wait), stop the thread. */
  void stop();
  bool active();

  /** The host reached the point where clients are served: app_start and the startup event. */
  void host_ready();

  /** Emit one event of the host (product "host"). */
  void emit(kind_e kind, std::string_view name, const nlohmann::json &props = nlohmann::json::object());
  void feature_used(std::string_view feature);

  /** The two choices may have changed (the config was saved): read them again and drop what the off kinds had waiting. */
  void consent_changed();
  /** Report the allow-listed settings that differ between two config snapshots as setting_changed. */
  void settings_changed(const std::map<std::string, std::string> &before, const std::map<std::string, std::string> &after);

  bool usage_enabled();
  bool errors_enabled();
  /** {install_id, session_id, endpoint, usage, errors}; nullopt while both choices are off (GET /api/telemetry/identity answers 403 then). */
  std::optional<nlohmann::json> identity();
  /** The install id, empty while both choices are off. */
  std::string install_id();
  /** The endpoint base, e.g. https://api.alkaidlab.com. */
  std::string endpoint();
  /** The two choices, the endpoint and the sender's counters (GET /api/telemetry/status). */
  nlohmann::json status();
  /** The events waiting in memory for the next batch, exactly as they would be sent (GET /api/telemetry/preview). */
  nlohmann::json preview();
  /** The Panel and the Web UI hand their events to the host (POST /api/telemetry/events): shaped against the page allow-list, consent applied, sent by this sender. -> events kept. */
  std::size_t ingest(std::string_view product, std::string_view version, const nlohmann::json &events);
  /** One blocking POST to `endpoint() + path` (for /v1/crash). -> HTTP status, 0 when nothing answered. Never call from a streaming thread. */
  int post_to(std::string_view path, std::string_view content_type, const std::string &body);

  // ---- streams (called from the stream code) ----
  struct stream_info_t {
    std::uint32_t session_id = 0;
    std::string client_uuid;  ///< only used to see a reconnect; never sent
    std::string client_family;
    std::string client_os;
    int video_format = -1;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool hdr = false;
    bool mic = false;
    bool host_audio = false;
    bool vdd = false;
    std::string app_name;  ///< only classified (desktop|bigscreen|sparxie|steam|other); the name is never sent
    std::string app_cmd;
    bool sparxie_app = false;  ///< the apps.json entry has a sparxie block (sparxie_app_of_apps)
    std::string sparxie_game;  ///< its game as a catalogue id; empty for any other app
    bool altp = false;  ///< the session runs over ALTP (no such flag on this branch line yet: the stream code passes false)
  };
  void stream_started(const stream_info_t &info);
  void stream_ended(std::uint32_t session_id, std::string_view stop_reason);
  void frame_encoded(std::uint32_t session_id) noexcept;
  void encode_ms(std::uint32_t session_id, double ms) noexcept;
  void capture_ms(std::uint32_t session_id, double ms) noexcept;
  /** What the host knows about the live sessions (loss, round trip, bitrate), read once per tick by the sender thread. */
  void set_session_probe(std::function<std::vector<session_probe_t>()> probe);

}  // namespace telemetry
