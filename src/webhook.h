/**
 * @file src/webhook.h
 * @brief Webhook notification system for Sunshine.
 */
#pragma once

#include <string>
#include <map>
#include <chrono>

namespace webhook {

  /**
   * @brief Webhook event types for different operations
   */
  enum class event_type_t {
    CONFIG_PIN_SUCCESS,    // 配置配对成功 / Config pairing successful
    CONFIG_PIN_FAILED,     // 配置配对失败 / Config pairing failed
    NV_APP_LAUNCH,         // NV应用启动 / NV application launched
    NV_APP_RESUME,         // NV应用恢复 / NV application resumed
    NV_APP_TERMINATE,      // NV应用终止 / NV application terminated
    NV_SESSION_START,      // NV会话开始 / NV session started
    NV_SESSION_END         // NV会话结束 / NV session ended
  };

  /**
   * @brief Webhook event data structure
   */
  struct event_t {
    event_type_t type;
    std::string alert_type;        // 告警类型 / Alert type
    std::string timestamp;
    std::string client_name;
    std::string client_ip;
    std::string server_ip;
    std::string app_name;
    std::int64_t app_id = 0;
    std::string session_id;
    std::map<std::string, std::string> extra_data;
  };

  /**
   * @brief Send webhook event asynchronously
   * @param event The webhook event to send
   */
  void send_event_async(const event_t& event);

  /**
   * @brief Send single webhook HTTP POST request
   * @param url Webhook URL
   * @param json_payload JSON payload to send
   * @param timeout_duration Request timeout
   * @return true if successful, false otherwise
   */
  bool send_single_webhook_request(const std::string& url, const std::string& json_payload, std::chrono::milliseconds timeout_duration);

  /**
   * @brief Check if webhook is enabled
   * @return true if webhook is enabled, false otherwise
   */
  bool is_enabled();

  /**
   * @brief Get localized alert message
   * @param type Event type
   * @param is_chinese Whether to use Chinese locale
   * @return Localized alert message
   */
  std::string get_alert_message(event_type_t type, bool is_chinese);

  /**
   * @brief Sanitize string for JSON (escape special characters)
   * @param str Input string
   * @return Sanitized string safe for JSON
   */
  std::string sanitize_json_string(const std::string& str);

  /**
   * @brief Get current timestamp in ISO format
   * @return ISO timestamp string
   */
  std::string get_current_timestamp();

  /**
   * @brief Generate detailed JSON payload for webhook
   * @param event Webhook event data
   * @param is_chinese Whether to use Chinese locale
   * @return JSON string for webhook payload
   */
  std::string generate_webhook_json(const event_t& event, bool is_chinese);

  /**
   * @brief Check if webhook sending is rate limited
   * @return true if rate limited, false otherwise
   */
  bool is_rate_limited();

  /**
   * @brief Record successful webhook send for rate limiting
   */
  void record_successful_send();

  /**
   * @brief Send rate limit exceeded notification
   */
  void send_rate_limit_notification();

  /**
   * @brief Check if we can create a new async thread
   * @return true if can create, false if thread limit reached
   */
  bool can_create_thread();

  /**
   * @brief Register a new thread (increment counter)
   */
  void register_thread();

  /**
   * @brief Unregister a thread (decrement counter)
   */
  void unregister_thread();

  /**
   * @brief 获取本地IP地址
   * @return 本地IP地址字符串，优先返回IPv4，其次IPv6，都获取不到返回空字符串
   */
  std::string get_local_ip();

}  // namespace webhook
