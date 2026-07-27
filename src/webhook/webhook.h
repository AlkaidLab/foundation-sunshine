/**
 * @file src/webhook/webhook.h
 * @brief Webhook notification system for Sunshine.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace webhook {
  inline constexpr std::chrono::milliseconds MIN_TIMEOUT {1000};
  inline constexpr std::chrono::milliseconds MAX_TIMEOUT {15000};
  inline constexpr std::chrono::milliseconds DEFAULT_TIMEOUT {5000};
  inline constexpr std::size_t MAX_URL_SIZE = 4096;
  inline constexpr int EVENT_TYPE_COUNT = 7;
  inline constexpr int MAX_TEST_RETRIES = 3;

  enum class event_type_t : std::uint8_t {
    CONFIG_PIN_SUCCESS = 0,
    CONFIG_PIN_FAILED = 1,
    NV_APP_LAUNCH = 2,
    NV_APP_RESUME = 3,
    NV_APP_TERMINATE = 4,
    NV_SESSION_START = 5,
    NV_SESSION_END = 6
  };

  struct event_t {
    event_type_t type;
    std::string timestamp;
    std::string client_name;
    std::string client_ip;
    std::string server_ip;
    std::string app_name;
    std::int64_t app_id = 0;
    std::string session_id;
    std::map<std::string, std::string> extra_data;
  };

  struct settings_t {
    std::string url;
    bool skip_ssl_verify = false;
    std::chrono::milliseconds timeout {DEFAULT_TIMEOUT};
  };

  struct configuration_t {
    bool enabled = false;
    std::string url;
    bool skip_ssl_verify = false;
    std::chrono::milliseconds timeout {DEFAULT_TIMEOUT};
    std::vector<int> events {0, 1, 2, 3, 4, 5, 6};
  };

  /**
   * Move-only token containing an immutable, validated configuration.
   *
   * Only prepare_configuration() can create a valid token. Callers can read
   * the value for persistence but cannot modify or forge the committed data.
   */
  class prepared_configuration_t {
  public:
    prepared_configuration_t(const prepared_configuration_t &) = delete;
    prepared_configuration_t &operator=(const prepared_configuration_t &) = delete;
    prepared_configuration_t(prepared_configuration_t &&) noexcept = default;
    prepared_configuration_t &operator=(prepared_configuration_t &&) noexcept = default;
    ~prepared_configuration_t() = default;

    explicit operator bool() const noexcept {
      return configuration_ != nullptr;
    }

    const configuration_t &value() const noexcept {
      return *configuration_;
    }

  private:
    friend prepared_configuration_t
    prepare_configuration(configuration_t configuration) noexcept;
    friend bool
    commit_configuration(prepared_configuration_t &&configuration) noexcept;

    prepared_configuration_t() noexcept = default;

    explicit prepared_configuration_t(
      std::shared_ptr<const configuration_t> configuration
    ) noexcept:
        configuration_(std::move(configuration)) {
    }

    std::shared_ptr<const configuration_t> configuration_;
  };

  enum class delivery_error_t {
    NONE,
    NOT_RUNNING,
    QUEUE_FULL,
    RATE_LIMITED,
    INVALID_URL,
    TRANSPORT,
    HTTP_STATUS,
    CANCELLED,
    INTERNAL
  };

  struct delivery_result_t {
    bool success = false;
    int http_status = 0;
    int attempts = 0;
    delivery_error_t error = delivery_error_t::INTERNAL;
  };

  using completion_handler_t = std::function<void(delivery_result_t)>;

  class deinit_t {
  public:
    deinit_t(const deinit_t &) = delete;
    deinit_t &operator=(const deinit_t &) = delete;
    ~deinit_t();

  private:
    friend std::unique_ptr<deinit_t> init() noexcept;
    deinit_t() = default;
  };

  /**
   * Establish the isolated Webhook lifecycle and try to start its runtime.
   *
   * The returned guard remains responsible for any later hot-start even when
   * the initial runtime start fails. A null guard only means lifecycle
   * ownership itself could not be established.
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init() noexcept;

  /** Idempotently start the runtime unless Sunshine shutdown has begun. */
  bool ensure_running() noexcept;

  /** Return whether the runtime is currently accepting deliveries. */
  bool runtime_active() noexcept;

  /** Validate a complete independently persisted Webhook configuration. */
  bool validate_configuration(const configuration_t &configuration) noexcept;

  /**
   * Validate, normalize and fully allocate a pending configuration.
   *
   * Preparing does not change the configuration visible to event producers,
   * so persistence can fail without exposing an unsaved destination.
   */
  prepared_configuration_t
  prepare_configuration(configuration_t configuration) noexcept;

  /**
   * Atomically publish a previously prepared configuration.
   *
   * The prepared object already owns all dynamic storage, keeping the commit
   * step independent from persistence and free of new allocations.
  */
  bool
  commit_configuration(prepared_configuration_t &&configuration) noexcept;

  /**
   * Atomically replace the configuration used for newly queued deliveries.
   *
   * Deliveries already accepted keep their previous immutable snapshot.
   */
  bool configure(configuration_t configuration) noexcept;

  /** Return a coherent copy of the currently active configuration. */
  configuration_t current_configuration();

  /** Queue a configured production event without blocking the caller. */
  void send_event_async(const event_t &event) noexcept;

  /**
   * Queue a test delivery using the same payload envelope and transport as
   * production events. retry_count is the number of additional attempts.
   */
  bool send_test_async(
    const settings_t &settings,
    int retry_count,
    completion_handler_t completion
  ) noexcept;

  bool is_enabled();
  bool is_event_enabled(event_type_t type) noexcept;
  int event_type_id(event_type_t type) noexcept;
  const char *event_type_name(event_type_t type) noexcept;
  std::string get_alert_message(event_type_t type, bool is_chinese);
  std::string sanitize_json_string(const std::string &str);
  std::string get_current_timestamp();
  std::string generate_webhook_json(const event_t &event, bool is_chinese);

  /** Stable, non-sensitive name for API responses and UI messages. */
  const char *delivery_error_name(delivery_error_t error) noexcept;

#ifdef SUNSHINE_TESTS
  namespace test_support {
    struct parsed_url_t {
      bool https = false;
      std::string server;
      std::string target;
    };

    bool parse_url(const std::string &url, parsed_url_t &parsed);
    std::string sanitize_header_value(const std::string &value);
    long timeout_seconds(std::chrono::milliseconds timeout) noexcept;
    bool is_success_status(int status) noexcept;
    bool is_retryable_status(int status) noexcept;
    std::optional<long> retry_after_seconds(const std::string &value);
  }  // namespace test_support
#endif

}  // namespace webhook
