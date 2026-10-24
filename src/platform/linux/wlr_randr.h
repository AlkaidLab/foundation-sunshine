/**
 * @file src/platform/linux/wlr_randr.h
 * @brief wlroots output management, spoken through the wlr-randr CLI.
 *
 * wlroots-based compositors (sway, labwc, wayfire, river, ...) publish
 * wlr-output-management-unstable-v1. That protocol has no D-Bus or IPC twin:
 * reading it means speaking Wayland, and Sunshine has no reason to link the
 * generated protocol code for a backend most builds will never use. The same
 * door the Plasma and niri backends already use is the right one here too -- a
 * small tool that understands the protocol and prints, verifies or applies a
 * whole output configuration.
 *
 * What makes this backend different from the other three:
 *   - it is the only one with no session signal to key off (`NIRI_SOCKET`,
 *     `XDG_CURRENT_DESKTOP` and a Plasma bus name all say "not me" here), so
 *     support is decided by an actual probe, cached with a cooldown so a
 *     compositor without the protocol costs one short probe per window rather
 *     than one 10 s wait per query;
 *   - wlr-randr also carries `--dryrun`, which asks the compositor to *test* a
 *     configuration atomically. That makes a change "verify first, then apply"
 *     instead of "apply, then look";
 *   - mirroring is not expressible: the protocol has no notion of two heads
 *     sharing a timing.
 */
#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace platf::wlr_randr {
  /// A query must answer within this window: the tool talks to a live
  /// compositor or fails, so waiting longer only delays the session.
  inline constexpr auto kProbeTimeout = std::chrono::milliseconds { 1500 };

  /// A change waits for the compositor to finish its own commit, which takes
  /// noticeably longer than printing a list.
  inline constexpr auto kChangeTimeout = std::chrono::milliseconds { 10'000 };

  /// After a failed probe, only retry once per this window.
  inline constexpr auto kRetryCooldown = std::chrono::seconds { 20 };

  struct exec_result_t {
    int exit_code { -1 };
    std::string output;
  };

  struct mode_t {
    /// The text wlr-randr accepts back as `--mode`, e.g. "1920x1080@59.94".
    std::string spec;
    unsigned int width { 0 };
    unsigned int height { 0 };
    double refresh { 0.0 };
    bool current { false };
    bool preferred { false };
  };

  struct head_t {
    std::string name;
    std::string description;
    bool enabled { false };
    unsigned int phys_width { 0 };
    unsigned int phys_height { 0 };
    int x { 0 };
    int y { 0 };
    double scale { 1.0 };
    /// "normal", "90", "flipped-270", ... -- whatever the tool printed.
    std::string transform { "normal" };
    std::vector<mode_t> modes;
  };

  struct state_t {
    std::vector<head_t> heads;
  };

  /**
   * @brief One `--output <name>` block of a configuration request.
   * @details Only the two verbs Sunshine actually needs are modelled. Enabling
   *          a head needs no mode: wlr-randr fills a disabled head in with its
   *          preferred mode, exactly like the "enable, then set the mode"
   *          sequence used against Plasma and niri.
   */
  struct request_t {
    std::string name;
    bool enabled { false };
    std::string mode;
  };

  /**
   * @brief Whether the wlr-randr tool is installed.
   * @details Checked in the usual binary directories instead of through PATH:
   *          the service environment is minimal and often has none.
   */
  bool
  available();

  /**
   * @brief Whether this session has a wlroots output manager behind it.
   * @details Probes once and remembers the answer; a failing probe arms the
   *          cooldown, so the verdict is stable between windows.
   */
  bool
  session_supports_wlr_randr();

  /**
   * @brief Whether a probe may be attempted now (cooldown not armed).
   */
  bool
  probe_allowed();

  /**
   * @brief Record an unavailable backend: arm the cooldown and log once.
   */
  void
  note_unavailable(const std::string &reason);

  /**
   * @brief Record a successful query: clear the cooldown.
   */
  void
  note_available();

  /**
   * @brief Read every head the compositor advertises.
   * @details An empty result means "no usable backend here", never "no
   *          outputs": that is what arms the cooldown.
   */
  state_t
  query(std::chrono::milliseconds timeout = kProbeTimeout);

  /**
   * @brief Parse the tool's listing.
   * @details Pure; exposed so the format contract can be tested without a
   *          compositor, which is the only way to cover it on a Plasma box.
   */
  state_t
  parse_state(const std::string &output);

  /**
   * @brief The `--mode` text for a timing, i.e. what the parser produces.
   */
  std::string
  mode_spec(unsigned int width, unsigned int height, double refresh);

  /**
   * @brief Whether a failed run means "this compositor has no such protocol".
   * @details The tool says one of two things when it cannot read a state: the
   *          compositor has no output manager, or there is no compositor to
   *          talk to. Both mean the same thing to a caller -- this session is
   *          not ours -- while any other failure is a real error worth
   *          reporting. Pure, so the exact strings the real binary prints can
   *          be pinned down by a test instead of by a comment.
   */
  bool
  failure_means_unsupported(const std::string &output);

  /**
   * @brief Build the argument list for a configuration request.
   * @param dry_run Ask the compositor to test the configuration only. It
   *                validates every head at once and changes nothing.
   */
  std::string
  build_args(const std::vector<request_t> &requests, bool dry_run = false);

  /**
   * @brief Run `wlr-randr <args>`, bounded like every other helper.
   */
  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout);
}  // namespace platf::wlr_randr
