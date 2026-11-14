/**
 * @file src/telemetry/events.h
 * @brief Telemetry v1 event shapes: the allow-list per event name, scrubbing, buckets and the batch envelope.
 *
 * Pure code (std + nlohmann::json only), so it is unit tested on its own. The contract is
 * docs/privacy_telemetry.md: unknown names and properties never leave the computer, free text is
 * scrubbed, and nothing that identifies a person (typed text, host or user names, paths, addresses) is carried.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace telemetry {

  /** The wire schema version of POST /v1/events. */
  inline constexpr int WIRE_VERSION = 1;
  /** Limits of one POST /v1/events request (contract). */
  inline constexpr std::size_t MAX_BATCH_EVENTS = 500;
  inline constexpr std::size_t MAX_BATCH_BYTES = 256 * 1024;
  /** The longest scrubbed message of an error event. */
  inline constexpr std::size_t MAX_MESSAGE_CHARS = 200;
  /** The longest message of a user_report (what the person wrote in the error dialog). */
  inline constexpr std::size_t MAX_USER_REPORT_CHARS = 2000;

  enum class kind_e {
    usage,
    error,
    experience,
  };

  /** "usage" | "error" | "experience". */
  std::string_view kind_name(kind_e kind);
  std::optional<kind_e> parse_kind(std::string_view text);

  /** The consent that governs a kind: usage and experience follow the usage statistics, error follows the error reports. */
  enum class consent_e {
    usage_statistics,
    error_reports,
  };
  consent_e consent_of(kind_e kind);

  struct event_t {
    std::int64_t t = 0;  ///< ms since the epoch
    kind_e kind = kind_e::usage;
    std::string name;
    nlohmann::json props = nlohmann::json::object();
    std::string product = "host";  ///< host | panel | web | sparxie-launch
    std::string version;  ///< product version when it differs from the host's (panel and web); empty = the host's
  };

  /** Products the host sender accepts events for. */
  bool known_product(std::string_view product);
  /** Event names a page (panel or web) may send through the host API; the host-only names (streams, quality, log summary) are refused so one stream is never counted twice. */
  bool client_may_send(std::string_view product, kind_e kind, std::string_view name);

  /**
   * Text with names, addresses, paths and secrets removed, no control characters, cut to `max` characters (counted as UTF-8 code points).
   * The rules follow foundation-ui scrubText (m3e src/telemetry/scrub.js).
   */
  std::string scrub_text(std::string_view text, std::size_t max = MAX_MESSAGE_CHARS);

  /** scrub_text for a line of the host log: whatever stands between quotes, brackets or braces is dropped first (names a person chose: a device, an app, a game). */
  std::string scrub_log_message(std::string_view text, std::size_t max = MAX_MESSAGE_CHARS);

  /** A product version as the Worker takes it (letters, digits, . _ + - space, at most 64 characters), else empty. */
  std::string safe_version(std::string_view version);
  /** An OS version string made only of letters, digits and . _ + - space, at most 48 characters, else empty. */
  std::string safe_os_version(std::string_view text);
  /** A language tag (zh, zh-CN, pt_BR), else empty. */
  std::string safe_locale(std::string_view text);

  /** A catalogue id: lower case letters, digits, '.', '_' and '-', 1 to `max` characters, starting with a letter or digit. Anything else -> empty. */
  std::string catalogue_id(std::string_view text, std::size_t max = 48);

  /**
   * Check an event against the allow-list of its name and rebuild it with only the allowed properties, each clamped or normalised.
   * @return nullopt when the name is not allowed for the kind, or a required property is missing or invalid.
   */
  std::optional<event_t> shape_event(kind_e kind, std::string_view name, const nlohmann::json &props, std::int64_t now_ms);

  /** The configuration keys whose changes are reported (setting_changed), and the value of a change reduced to the allow-listed enum or bucket ("other" when it is not one). Empty key -> not reported. */
  bool setting_key_allowed(std::string_view key);
  std::string setting_value_bucket(std::string_view key, std::string_view value);
  const std::vector<std::string_view> &setting_keys();

  /** Bucket of a bitrate in kbps for stream_quality (rounded to a lower bound step) so a value cannot single a person out. */
  std::int64_t round_to_step(double value, std::int64_t step);

  /** What every batch carries about this install (contract: envelope). All text is a catalogue-like short string. */
  struct context_t {
    std::string install_id;
    std::string session_id;
    std::string version;
    std::string channel;
    std::string os;
    std::string os_version;
    std::string arch;
    std::string gpu_vendor;
    std::string locale;
  };

  /** The fields every batch carries besides its events, as they are sent (what the person sees in "see what will be sent"). */
  nlohmann::json envelope_fields(const context_t &context, std::string_view product, const std::string &version);

  /**
   * Batches for one product: events of that product are split in as many envelopes as the 500 events and 256 KB limits need.
   * `events` must all carry `product`; a product version on an event (panel, web) replaces the context's version.
   */
  std::vector<nlohmann::json> build_envelopes(const context_t &context, std::string_view product, const std::vector<event_t> &events);

  /** event <-> json (the form kept in the disk queue and sent in a batch). */
  nlohmann::json event_to_json(const event_t &event);
  std::optional<event_t> event_from_json(const nlohmann::json &json);

  /** Percentiles over samples (nearest rank on a sorted copy). Empty input -> 0. p in [0, 100]. */
  double percentile(std::vector<double> samples, double p);

  /** A fixed-size histogram for latency-like values, so a stream of any length keeps O(1) memory. Not thread safe. */
  class histogram_t {
  public:
    histogram_t(double max_value = 2000.0, std::size_t buckets = 400);
    void add(double value);
    std::uint64_t count() const { return count_; }
    /** p in [0, 100]; the upper edge of the bucket holding that rank. 0 when empty. */
    double percentile(double p) const;
    void clear();

  private:
    double max_;
    double width_;
    std::vector<std::uint32_t> bins_;
    std::uint64_t count_ = 0;
  };

}  // namespace telemetry
