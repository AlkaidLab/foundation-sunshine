/**
 * @file src/telemetry/opt_out.h
 * @brief The one record that says a choice was turned off.
 *
 * When a person turns off usage statistics or error reports, each product this install has seen (the host, and the panel and the web interface when they have talked to it) sends exactly one small record:
 * which of the two choices, the product and its version, under the install's anonymous id. Nothing else leaves afterwards. The record lets the data tell "the person turned it off" from "our pipeline broke":
 * the Worker counts the installs that turned a choice off per day and does not count the record as activity.
 *
 * Precedent: Mozilla Glean sends one `deletion-request` ping when upload is disabled (it carries the client id, is retried until it is delivered, is not sent when a client starts with upload already
 * disabled), and Firefox Telemetry sends the same ping; both keep it free of metrics. This is the same shape with less in it. The record is written to disk first and retried
 * (bounded: 20 tries, 14 days), so a person who is offline when they turn the choice off is still counted once they are online.
 *
 * Pure code (std + nlohmann::json), unit tested on its own.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "events.h"

namespace telemetry::opt_out {

  inline constexpr std::size_t MAX_RECORDS = 16;
  inline constexpr int MAX_TRIES = 20;
  inline constexpr std::int64_t MAX_AGE_MS = 14LL * 24 * 3600 * 1000;
  inline constexpr std::int64_t RETRY_BASE_MS = 10 * 60 * 1000;
  inline constexpr std::size_t MAX_FILE_BYTES = 16 * 1024;

  struct record_t {
    std::string choice;  ///< "usage" | "errors"
    std::string product;  ///< host | panel | web | sparxie-launch
    std::string version;
    std::string install_id;  ///< the id the install had when the choice was turned off (text UUID)
    std::int64_t t = 0;  ///< when it was turned off (ms since the epoch)
    std::int64_t next_try_ms = 0;
    int tries = 0;
    std::uint64_t id = 0;  ///< random, never reused: a send result is matched to its record by this, even when t repeats after a clock jump
  };

  /** What the host remembers of the two choices between starts: a turn-off is what changed since the last time it looked. */
  struct state_t {
    bool known = false;  ///< false on the first run ever: a client that starts with a choice already off sends nothing (nothing was turned off while it ran)
    bool usage = true;
    bool errors = true;
  };

  nlohmann::json state_to_json(const state_t &state);
  state_t state_from_json(const nlohmann::json &json);

  /** "usage" and/or "errors": the choices that were on in `before` and are off now. */
  std::vector<std::string> turned_off(const state_t &before, bool usage_now, bool errors_now);

  /** The products to tell: the host and every product that has talked to it (name -> version). */
  struct product_version_t {
    std::string product;
    std::string version;
  };

  std::vector<record_t> parse(std::string_view text);
  std::string serialize(const std::vector<record_t> &records);

  /** Adds one record per product and choice; a record for the same install, choice and product that is waiting already is not added twice. */
  void add(std::vector<record_t> &records, const std::vector<std::string> &choices, const std::vector<product_version_t> &products, const std::string &install_id, std::int64_t now_ms);

  /** The records that may be tried now; the others stay. Drops what is too old or was tried too often. */
  std::vector<std::size_t> due(std::vector<record_t> &records, std::int64_t now_ms);

  /** After a failed try: the next time grows (10 min, 20, 40 ... at most one day). */
  void failed(record_t &record, std::int64_t now_ms);

  /**
   * The body of /v1/events for one record: the install's id, one fresh session number, the product and its version, and one event. Nothing about the computer (no channel, system, architecture,
   * graphics vendor or language) goes with it. Null when the event does not pass the allow-list.
   */
  nlohmann::json body_of(const record_t &record, std::string_view session_id);

  bool is_uuid(std::string_view text);

}  // namespace telemetry::opt_out
