/**
 * @file src/confighttp/session_store.h
 * @brief Server-side Web UI sessions with hashed identifiers and CSRF binding.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace confighttp::sessions {
  using clock_t = std::chrono::system_clock;
  using time_point_t = clock_t::time_point;

  /**
   * Idle and absolute lifetimes. Remember-me sessions use a longer pair so a
   * browser can survive a reboot without weakening the short default.
   *
   * Sources: OWASP ASVS 4.0.3 V3.3.2 (30 days, or 12 hours / 30 minutes idle)
   * [ds5-owasp-asvs4-v3-20261103]; OWASP Session Management Cheat Sheet,
   * Session ID Life Cycle and Session Expiration [p0-owasp-session-20261109].
   */
  struct policy_t {
    std::chrono::seconds idle { std::chrono::minutes(30) };
    std::chrono::seconds absolute { std::chrono::hours(12) };
    std::chrono::seconds remember_idle { std::chrono::hours(24 * 7) };
    std::chrono::seconds remember_absolute { std::chrono::hours(24 * 30) };
  };

  struct client_t {
    std::string address;
    std::string user_agent;
  };

  struct session_t {
    std::string public_id;
    std::string username;
    time_point_t created_at;
    time_point_t last_seen;
    time_point_t expires_at;
    time_point_t absolute_expires_at;
    bool remember = false;
  };

  struct created_session_t: session_t {
    // Returned exactly once, to the response that creates the session. Only
    // hashes of both values are retained by the store.
    std::string token;
    std::string csrf_token;
  };

  struct resumed_session_t: created_session_t {};

  struct summary_t: session_t {
    std::string masked_address;
    std::string short_user_agent;
    bool current = false;
  };

  /**
   * Session table with an in-memory working set and an optional durable
   * remember-me store.
   *
   * The store keeps only SHA-256 verifiers for the session token and the CSRF
   * token, never the values a browser holds. Only remember-me records are
   * written to the restricted persistence path; browser sessions remain
   * process-local.
   *
   * Sources: OWASP ASVS 4.0.3 V3.2.1, V3.2.2, V3.4 [ds5-owasp-asvs4-v3-20261103];
   * OWASP Session Management Cheat Sheet, Server-Side Session Token Storage,
   * Session Expiration and Logout [p0-owasp-session-20261109].
   */
  class store_t {
  public:
    explicit store_t(policy_t policy = {}, std::function<time_point_t()> clock = {});

    using persistence_writer_t = std::function<bool(const std::filesystem::path &, std::string_view)>;

    /** Select the configuration-directory persistence file and load it once. */
    void
    configure(policy_t policy, std::filesystem::path persistence_path = {}, persistence_writer_t writer = {});

    /**
     * Create a session and, when rotate_from names a live token, revoke that
     * token first. A new session is always created, so authentication never
     * reuses an identifier supplied by the client.
     */
    std::optional<created_session_t>
    create(std::string username, bool remember, client_t client,
      std::string credential_fingerprint,
      std::optional<std::string_view> rotate_from = std::nullopt);

    /**
     * Validate a token and refresh its idle deadline. Returns nullopt for
     * unknown, idle-expired, absolute-expired or credential-stale sessions.
     */
    std::optional<session_t>
    lookup(std::string_view token, std::string_view credential_fingerprint);

    /**
     * Resume a remember-me session after the browser presents its cookie.
     * The token and CSRF verifier are rotated and the new cookie values are
     * returned. Browser-session records return nullopt.
     */
    std::optional<resumed_session_t>
    resume(std::string_view token, std::string_view credential_fingerprint);

    /**
     * Validate a state-changing request that authenticated with a cookie. The
     * header must match the server-side verifier and the readable cookie
     * (double submit); both comparisons are constant time.
     */
    bool
    csrf_matches(std::string_view token, std::string_view header, std::string_view cookie) const;

    /**
     * Bind a live session to a new credential fingerprint. Used after a
     * password change so the initiating session survives while every other
     * session is revoked.
     */
    bool
    rekey(std::string_view token, std::string_view credential_fingerprint);

    bool
    revoke(std::string_view token);

    std::size_t
    revoke_all(std::optional<std::string_view> keep = std::nullopt);

    bool
    revoke_public(std::string_view public_id);

    std::vector<summary_t>
    list(std::string_view current_token);

    std::size_t
    purge_expired();

    void
    clear(bool persist = true);

    std::size_t
    size() const;

  private:
    struct record_t {
      std::string token_hash;
      std::string csrf_hash;
      std::string username;
      time_point_t created_at;
      time_point_t last_seen;
      time_point_t idle_expires_at;
      time_point_t absolute_expires_at;
      std::string credential_hash;
      std::string masked_address;
      std::string short_user_agent;
      bool remember = false;
    };

    static constexpr std::size_t maximum_sessions = 128;

    time_point_t
    now() const;

    record_t *
    find_locked(std::string_view token_hash, std::string_view credential_fingerprint, time_point_t at);

    void
    enforce_capacity_locked();

    void
    load_locked();

    bool
    persist_locked() const;

    policy_t policy_;
    std::function<time_point_t()> clock_;
    std::filesystem::path persistence_path_;
    persistence_writer_t persistence_writer_;
    bool persistence_loaded_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, record_t> records_;
  };
}  // namespace confighttp::sessions
