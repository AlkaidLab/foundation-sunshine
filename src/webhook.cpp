/**
 * @file src/webhook.cpp
 * @brief Webhook notification system implementation for Sunshine.
 */

#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <regex>
#include <mutex>
#include <algorithm>
#include <thread>
#include <atomic>
#include <map>
#include <cstdlib>
#include <Simple-Web-Server/client_http.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include "config.h"
#include "logging.h"
#include "httpcommon.h"
#include "utility.h"
#include "platform/common.h"
#include "webhook_httpsclient.h"
#include "webhook.h"
#include "webhook_format.h"
#include "network.h"

using namespace std::literals;

namespace webhook {
  // Rate limiting variables
  static std::vector<std::chrono::system_clock::time_point> successful_sends;
  static std::mutex rate_limit_mutex;
  static const int MAX_SENDS_PER_MINUTE = 10;
  static const int RATE_LIMIT_WINDOW_MINUTES = 1;
  static bool rate_limit_notification_sent = false;

  std::string generate_signature(long long timestamp, const std::string& hostname)
  {
    // 使用简单的哈希算法生成签名
    std::string data = hostname + std::to_string(timestamp) + "Sunshine_Foundation";
    std::hash<std::string> hasher;
    size_t hash_value = hasher(data);
    return std::to_string(hash_value);
  }

  SimpleWeb::CaseInsensitiveMultimap generate_webhook_headers()
  {
    SimpleWeb::CaseInsensitiveMultimap headers;
    // 生成时间戳和签名
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string hostname = platf::get_host_name();
    std::string signature = generate_signature(timestamp, hostname);
    // 校验相关的请求头
    headers.emplace("X-Timestamp", std::to_string(timestamp));
    headers.emplace("X-Hostname", hostname);
    headers.emplace("X-Signature", signature);
    headers.emplace("X-Client-ID", "Sunshine_Foundation");
    headers.emplace("X-Auth-Token", "Sunshine_Foundation_" + std::to_string(timestamp % 10000));
    headers.emplace("X-API-Version", "v1.0");
    headers.emplace("X-Client-Info", "Sunshine Foundation");
    headers.emplace("X-Trace-ID", "sf_" + std::to_string(timestamp) + "_" + std::to_string(rand() % 1000));
    headers.emplace("X-Service-Name", "Sunshine_Foundation_Service");
    headers.emplace("X-Component", "Sunshine_Foundation_Component");
    headers.emplace("User-Agent", "Sunshine_Foundation/1.0 (System Notification Service)");
    headers.emplace("Content-Type", "application/json");
    return headers;
  }

  /**
   * @brief 获取本地IP地址
   * @return 本地IP地址字符串，优先返回IPv4，其次IPv6，都获取不到返回空字符串
   */
  std::string get_local_ip() {
    try {
      boost::asio::io_context io_context;
      boost::asio::ip::tcp::resolver resolver(io_context);
      auto results = resolver.resolve(boost::asio::ip::host_name(), "");
      
      std::string ipv4_address = "";
      std::string ipv6_address = "";
      
      for (auto it = results.begin(); it != results.end(); ++it) {
        boost::asio::ip::tcp::endpoint ep = *it;
        auto address = ep.address();
        if (!address.is_loopback()) {
          auto normalized_address = net::normalize_address(address);
          auto address_str = normalized_address.to_string();
          
          if (normalized_address.is_v4() && ipv4_address.empty()) {
            ipv4_address = address_str;
          } else if (normalized_address.is_v6() && ipv6_address.empty()) {
            ipv6_address = address_str;
          }
        }
      }
      
      // 优先返回IPv4，其次IPv6
      if (!ipv4_address.empty()) {
        return ipv4_address;
      } else if (!ipv6_address.empty()) {
        return ipv6_address;
      }
    } catch (const std::exception& e) {
      BOOST_LOG(debug) << "Webhook: Failed to get local IP: " << e.what();
    }
    return "";
  }

  // Thread management variables
  static std::atomic<int> active_thread_count{0};
  static std::mutex thread_mutex;
  static const int MAX_CONCURRENT_THREADS = 10;

  /**
   * @brief 发送webhook请求
   * @param url Webhook URL
   * @param json_payload JSON payload to send
   * @param timeout_duration Request timeout
   * @return true if successful, false otherwise
   */
  bool send_webhook_request(const std::string& url, const std::string& json_payload, std::chrono::milliseconds timeout_duration)
  {
    const int max_retries = 2; // Maximum 2 retries, total 3 attempts
    
    for (int attempt = 1; attempt <= max_retries + 1; ++attempt) {
      BOOST_LOG(debug) << "Webhook attempt " << attempt << "/" << (max_retries + 1) << ": " << json_payload;
      
      if (send_single_webhook_request(url, json_payload, timeout_duration)) {
        if (attempt > 1) {
          BOOST_LOG(info) << "Webhook succeeded on attempt " << attempt;
        }
        return true;
      }
      
      if (attempt <= max_retries) {
        BOOST_LOG(warning) << "Webhook attempt " << attempt << " failed, retrying...";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Wait 1 second before retry
      }
    }
    
    BOOST_LOG(error) << "Webhook failed after " << (max_retries + 1) << " attempts";
    return false;
  }

  /**
   * @brief 发送单个webhook请求
   * @param url Webhook URL
   * @param json_payload JSON payload to send
   * @param timeout_duration Request timeout
   * @return true if successful, false otherwise
   */
  bool send_single_webhook_request(const std::string& url, const std::string& json_payload, std::chrono::milliseconds timeout_duration)
  {
    try {
      // Parse URL to determine protocol and host
      std::string parsed_url = url;
      bool is_https = (parsed_url.find("https://") == 0);
      
      if (is_https) {
        parsed_url = parsed_url.substr(8); // Remove "https://"
      } else if (parsed_url.find("http://") == 0) {
        parsed_url = parsed_url.substr(7); // Remove "http://"
      }
      
      // Find path separator
      size_t path_pos = parsed_url.find('/');
      std::string host_port = parsed_url.substr(0, path_pos);
      std::string path = (path_pos != std::string::npos) ? parsed_url.substr(path_pos) : "/";
      
      if (is_https) {
        // Use HTTPS client with SSL verification control
        bool verify_certificate = !config::webhook.skip_ssl_verify;
        WebhookHttpsClient client(host_port, verify_certificate, host_port);
        client.config.timeout = timeout_duration.count() / 1000; // Convert to seconds
        try{
          auto headers = generate_webhook_headers();
          auto response = client.request("POST", path, json_payload, headers);
          return (response->status_code == "200 OK");
        } catch (const std::exception& e) {
          BOOST_LOG(warning) << "Webhook HTTPS request error: " << e.what();
          return false;
        }
      } 
      else {
        // Use HTTP client
        SimpleWeb::Client<SimpleWeb::HTTP> client(host_port);
        client.config.timeout = timeout_duration.count() / 1000; // Convert to seconds
        try{
          auto headers = generate_webhook_headers();
          auto response = client.request("POST", path, json_payload, headers);
          return (response->status_code == "200 OK");
        } catch (const std::exception& e) {
          BOOST_LOG(warning) << "Webhook HTTP request error: " << e.what();
          return false;
        }
      }
    } catch (const std::exception& e) {
      std::string error_msg = e.what();
      BOOST_LOG(warning) << "Webhook request error: " << error_msg;
      return false;
    }
  }


  /**
   * @brief 异步发送webhook事件
   * @param event Webhook事件数据
   */
  void send_event_async(const event_t& event)
  {
    // Check if webhook is enabled
    if (!is_enabled()) {
      return;
    }

    // Initialize webhook format if not already done
    static bool format_initialized = false;
    if (!format_initialized) {
      // 默认配置为webhook格式
      configure_webhook_format(true);
      format_initialized = true;
    }

    // Check thread limit and register thread atomically
    if (!can_create_thread()) {
      BOOST_LOG(warning) << "Webhook thread limit reached (" << MAX_CONCURRENT_THREADS << "), skipping send";
      return;
    }
    
    // Register thread before creating it
    register_thread();

    // Check rate limiting
    if (is_rate_limited()) {
      BOOST_LOG(warning) << "Webhook rate limited, skipping send";
      unregister_thread(); // Unregister since we're not creating the thread
      send_rate_limit_notification();
      return;
    }

    // Run in separate thread to avoid blocking
    std::thread([event]() {
      try {
        // Determine locale
        bool is_chinese = (config::sunshine.locale == "zh" || config::sunshine.locale == "zh_TW");
        
        // Generate detailed JSON payload based on event type using format config
        std::string json_payload = g_webhook_format.generate_json_payload(event, is_chinese);
        
        BOOST_LOG(debug) << "Sending webhook: " << json_payload;

        // Create timeout controller
        auto timeout_duration = config::webhook.timeout;
        
        // Send HTTP POST request using Simple-Web-Server client
        bool success = send_webhook_request(config::webhook.url, json_payload, timeout_duration);
        
        if (success) {
          BOOST_LOG(info) << "Webhook sent successfully";
          record_successful_send(); // Record successful send for rate limiting
        } else {
          BOOST_LOG(warning) << "Failed to send webhook";
        }
        
      } catch (const std::exception& e) {
        BOOST_LOG(error) << "Webhook error: " << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Webhook unknown error occurred";
      }
      
      // Always unregister thread when done
      unregister_thread();
    }).detach();
  }

  /**
   * @brief 检查webhook是否启用
   * @return true if enabled, false otherwise
   */
  bool is_enabled()
  {
    return config::webhook.enabled && !config::webhook.url.empty();
  }

  /**
   * @brief 获取告警消息
   * @param type Webhook事件类型
   * @param is_chinese 是否使用中文
   * @return 告警消息
   */
  std::string get_alert_message(event_type_t type, bool is_chinese)
  {
    switch (type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
        return is_chinese ? "🔗 配置配对成功" : "🔗 Config pairing successful";
      case event_type_t::CONFIG_PIN_FAILED:
        return is_chinese ? "❌ 配置配对失败" : "❌ Config pairing failed";
      case event_type_t::NV_APP_LAUNCH:
        return is_chinese ? "🚀 应用启动" : "🚀 application launched";
      case event_type_t::NV_APP_RESUME:
        return is_chinese ? "▶️ 应用恢复" : "▶️ application resumed";
      case event_type_t::NV_APP_TERMINATE:
        return is_chinese ? "⏹️ 应用终止" : "⏹️ application terminated";
      case event_type_t::NV_SESSION_START:
        return is_chinese ? "📱 会话开始" : "📱 session started";
      case event_type_t::NV_SESSION_END:
        return is_chinese ? "📱 会话结束" : "📱 session ended";
      default:
        return is_chinese ? "🔔 系统通知" : "🔔 System notification";
    }
  }

  /**
   * @brief 清理JSON字符串
   * @param str 原始字符串
   * @return 清理后的字符串
   */
  std::string sanitize_json_string(const std::string& str)
  {
    std::string result = str;
    
    // Escape backslashes first
    result = std::regex_replace(result, std::regex(R"(\\)"), "\\\\");
    
    // Escape double quotes
    result = std::regex_replace(result, std::regex(R"(")"), "\\\"");
    
    // Escape newlines
    result = std::regex_replace(result, std::regex(R"(\n)"), "\\n");
    
    // Escape carriage returns
    result = std::regex_replace(result, std::regex(R"(\r)"), "\\r");
    
    // Escape tabs
    result = std::regex_replace(result, std::regex(R"(\t)"), "\\t");
    
    // Escape other control characters (0x00-0x1F except \n, \r, \t)
    result = std::regex_replace(result, std::regex(R"([\x00-\x08\x0B\x0C\x0E-\x1F])"), "");
    
    return result;
  }

  /**
   * @brief 获取当前时间戳
   * @return 当前时间戳
   */
  std::string get_current_timestamp()
  {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    return oss.str();
  }

  /**
   * @brief 检查是否达到速率限制
   * @return true if rate limited, false otherwise
   */
  bool is_rate_limited()
  {
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto window_start = now - std::chrono::minutes(RATE_LIMIT_WINDOW_MINUTES);
    
    // Remove old entries (older than rate limit window)
    successful_sends.erase(
      std::remove_if(successful_sends.begin(), successful_sends.end(),
        [window_start](const std::chrono::system_clock::time_point& time) {
          return time < window_start;
        }),
      successful_sends.end()
    );
    
    return successful_sends.size() >= MAX_SENDS_PER_MINUTE;
  }

  /**
   * @brief 记录成功发送
   */
  void record_successful_send() {
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    successful_sends.push_back(std::chrono::system_clock::now());
  }

  /**
   * @brief 发送速率限制通知
   */
  void send_rate_limit_notification()
  {
    if (rate_limit_notification_sent) {
      return; // Only send once per rate limit period
    }
    
    rate_limit_notification_sent = true;
    
    // Reset the flag after rate limit window
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::minutes(RATE_LIMIT_WINDOW_MINUTES));
      rate_limit_notification_sent = false;
    }).detach();
    
    // Send rate limit notification
    bool is_chinese = (config::sunshine.locale == "zh" || config::sunshine.locale == "zh_TW");
    std::string hostname = platf::get_host_name();
    std::string local_ip = get_local_ip();
    std::string ip_info = local_ip.empty() ? "" : local_ip;
    std::string message = is_chinese ? 
      "主机: " + hostname + " " + ip_info + "\n ⚠️ Webhook 发送频率过高，已限制发送\n最近" + std::to_string(RATE_LIMIT_WINDOW_MINUTES) + "分钟内发送次数超过" + std::to_string(MAX_SENDS_PER_MINUTE) + "次\n时间: " + get_current_timestamp() :
      "Host: " + hostname + " " + ip_info + "\n ⚠️ Webhook sending rate too high, sending limited\nExceeded " + std::to_string(MAX_SENDS_PER_MINUTE) + " sends in the last " + std::to_string(RATE_LIMIT_WINDOW_MINUTES) + " minute(s)\nTime: " + get_current_timestamp();
    
    std::ostringstream json_stream;
    json_stream << "{";
    json_stream << "\"msgtype\":\"text\",";
    json_stream << "\"hostname\":\"" << sanitize_json_string(platf::get_host_name()) << "\",";
    json_stream << "\"text\":{";
    json_stream << "\"content\":\"" << sanitize_json_string(message) << "\"";
    json_stream << "}";
    json_stream << "}";
    
    std::string json_payload = json_stream.str();
    
    // Send the rate limit notification (special case - bypass thread limit)
    std::thread([json_payload]() {
      try {
        send_single_webhook_request(config::webhook.url, json_payload, config::webhook.timeout);
      } catch (const std::exception& e) {
        BOOST_LOG(error) << "Failed to send rate limit notification: " << e.what();
      } catch (...) {
        BOOST_LOG(error) << "Unknown error in rate limit notification";
      }
    }).detach();
  }

  /**
   * @brief 检查是否可以创建线程
   * @return true if can create, false otherwise
   */
  bool can_create_thread()
  {
    return active_thread_count.load() < MAX_CONCURRENT_THREADS;
  }

  /**
   * @brief 注册线程
   */
  void register_thread()
  {
    active_thread_count.fetch_add(1);
    BOOST_LOG(debug) << "Webhook thread registered, active threads: " << active_thread_count.load();
  }

  /**
   * @brief 注销线程
   */
  void unregister_thread()
  {
    int count = active_thread_count.fetch_sub(1);
    BOOST_LOG(debug) << "Webhook thread unregistered, active threads: " << (count - 1);
  }

}  // namespace webhook
