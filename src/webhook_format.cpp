/**
 * @file src/webhook_format.cpp
 * @brief Webhook格式配置和模板实现
 */
#include "webhook_format.h"
#include "webhook.h"
#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include <sstream>
#include <regex>

namespace webhook {

  // 全局webhook格式实例
  WebhookFormat g_webhook_format;

  WebhookFormat::WebhookFormat(format_type_t format_type)
    : format_type_(format_type)
    , use_colors_(true)
    , simplify_ip_(true)
    , time_format_("%Y-%m-%d %H:%M:%S")
  {
  }

  void WebhookFormat::set_format_type(format_type_t format_type) {
    format_type_ = format_type;
  }

  format_type_t WebhookFormat::get_format_type() const {
    return format_type_;
  }

  void WebhookFormat::set_custom_template(event_type_t event_type, const std::string& template_str) {
    custom_templates_[event_type] = template_str;
  }

  void WebhookFormat::set_use_colors(bool use_colors) {
    use_colors_ = use_colors;
  }

  void WebhookFormat::set_simplify_ip(bool simplify_ip) {
    simplify_ip_ = simplify_ip;
  }

  void WebhookFormat::set_time_format(const std::string& time_format) {
    time_format_ = time_format;
  }

  std::string WebhookFormat::format_ip_address(const std::string& ip) const {
    if (ip.empty()) return "";
    if (!simplify_ip_) {
      return ip;
    }
    // 处理IPv6地址
    if (ip.find(':') != std::string::npos) {
      // 简化IPv6显示
      if (ip.find("fe80::") == 0) {
        return "IPv6 (本地链路)";
      } else if (ip.find("::1") != std::string::npos) {
        return "IPv6 (回环)";
      } else {
        return "IPv6";
      }
    }
    
    // IPv4地址直接返回
    return ip;
  }

  std::string WebhookFormat::format_timestamp(const std::string& timestamp) const
  {
    // 将 ISO 8601 格式转换为更友好的格式
    // 2025-10-07T16:36:33.595 -> 2025-10-07 16:36:33
    std::string formatted = timestamp;
    size_t dot_pos = formatted.find('.');
    if (dot_pos != std::string::npos) {
      formatted = formatted.substr(0, dot_pos);
    }
    size_t t_pos = formatted.find('T');
    if (t_pos != std::string::npos) {
      formatted[t_pos] = ' ';
    }
    return formatted;
  }

  std::string WebhookFormat::get_event_color(event_type_t event_type) const
  {
    if (!use_colors_) {
      return "";
    }

    switch (event_type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
      case event_type_t::NV_APP_LAUNCH:
      case event_type_t::NV_APP_RESUME:
      case event_type_t::NV_SESSION_START:
        return colors::COLOR_INFO;
        
      case event_type_t::CONFIG_PIN_FAILED:
      case event_type_t::NV_APP_TERMINATE:
        return colors::COLOR_WARNING;
        
      case event_type_t::NV_SESSION_END:
        return colors::COLOR_COMMENT;
        
      default:
        return colors::COLOR_COMMENT;
    }
  }

  std::string WebhookFormat::get_event_title(event_type_t event_type, bool is_chinese) const
  {
    switch (event_type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
        return is_chinese ? "配置配对成功" : "Config Pairing Successful";
      case event_type_t::CONFIG_PIN_FAILED:
        return is_chinese ? "配置配对失败" : "Config Pairing Failed";
      case event_type_t::NV_APP_LAUNCH:
        return is_chinese ? "应用启动" : "Application Launched";
      case event_type_t::NV_APP_RESUME:
        return is_chinese ? "应用恢复" : "Application Resumed";
      case event_type_t::NV_APP_TERMINATE:
        return is_chinese ? "应用终止" : "Application Terminated";
      case event_type_t::NV_SESSION_START:
        return is_chinese ? "会话开始" : "Session Started";
      case event_type_t::NV_SESSION_END:
        return is_chinese ? "会话结束" : "Session Ended";
      default:
        return is_chinese ? "系统通知" : "System Notification";
    }
  }

  std::string WebhookFormat::generate_markdown_content(const event_t& event, bool is_chinese) const
  {
    std::ostringstream content_stream;
    // 获取主机信息
    std::string hostname = platf::get_host_name();
    std::string local_ip = get_local_ip();
    std::string formatted_ip = format_ip_address(local_ip);
    content_stream << (is_chinese ? "**Sunshine系统通知**" : "**Sunshine System Notification**") << "\n\n";
    
    // 根据事件类型设置不同的颜色和内容
    std::string event_title = get_event_title(event.type, is_chinese);
    std::string event_color = get_event_color(event.type);
    
    if (use_colors_ && !event_color.empty()) {
      content_stream << "<font color=\"" << event_color << "\">**" << event_title << "**</font>\n\n";
    } else {
      content_stream << "**" << event_title << "**\n\n";
    }
    // 添加基本信息
    content_stream << ">主机名:<font color=\"comment\">" << hostname << "</font>\n";
    if (!formatted_ip.empty()) {
      content_stream << ">IP地址:<font color=\"comment\">" << formatted_ip << "</font>\n";
    }
    // 添加事件特定信息
    switch (event.type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
      case event_type_t::CONFIG_PIN_FAILED: {
        if (!event.client_name.empty()) {
          content_stream << ">客户端名称:<font color=\"comment\">" << event.client_name << "</font>\n";
        }
        if (!event.client_ip.empty()) {
          content_stream << ">客户端IP:<font color=\"comment\">" << event.client_ip << "</font>\n";
        }
        if (!event.server_ip.empty()) {
          content_stream << ">服务器IP:<font color=\"comment\">" << event.server_ip << "</font>\n";
        }
        break;
      }
      case event_type_t::NV_APP_LAUNCH:
      case event_type_t::NV_APP_RESUME:
      case event_type_t::NV_APP_TERMINATE: {
        if (!event.app_name.empty()) {
          content_stream << ">应用名称:<font color=\"comment\">" << event.app_name << "</font>\n";
        }
        if (event.app_id > 0) {
          content_stream << ">应用ID:<font color=\"comment\">" << event.app_id << "</font>\n";
        }
        if (!event.client_name.empty()) {
          content_stream << ">客户端:<font color=\"comment\">" << event.client_name << "</font>\n";
        }
        if (!event.client_ip.empty()) {
          content_stream << ">客户端IP:<font color=\"comment\">" << event.client_ip << "</font>\n";
        }
        if (!event.server_ip.empty()) {
          content_stream << ">服务器IP:<font color=\"comment\">" << event.server_ip << "</font>\n";
        }
        // 添加额外信息
        for (const auto& [key, value] : event.extra_data) {
          if (key == "resolution") {
            content_stream << ">分辨率:<font color=\"comment\">" << value << "</font>\n";
          } else if (key == "fps") {
            content_stream << ">帧率:<font color=\"comment\">" << value << "</font>\n";
          } else if (key == "host_audio") {
            content_stream << ">音频:<font color=\"comment\">" 
                          << (value == "true" ? (is_chinese ? "启用" : "Enabled") : (is_chinese ? "禁用" : "Disabled")) << "</font>\n";
          }
        }
        break;
      }
      case event_type_t::NV_SESSION_START:
      case event_type_t::NV_SESSION_END: {
        if (!event.app_name.empty()) {
          content_stream << ">应用名称:<font color=\"comment\">" << event.app_name << "</font>\n";
        }
        if (!event.client_name.empty()) {
          content_stream << ">客户端:<font color=\"comment\">" << event.client_name << "</font>\n";
        }
        if (!event.session_id.empty()) {
          content_stream << ">会话ID:<font color=\"comment\">" << event.session_id << "</font>\n";
        }
        break;
      }
      default:
        break;
    }
    content_stream << ">时间:<font color=\"comment\">" << format_timestamp(event.timestamp) << "</font>";
    // 添加错误信息
    auto error_it = event.extra_data.find("error");
    if (error_it != event.extra_data.end()) {
      content_stream << "\n>错误信息:<font color=\"warning\">" << error_it->second << "</font>";
    }
    return content_stream.str();
  }

  std::string WebhookFormat::generate_text_content(const event_t& event, bool is_chinese) const {
    std::ostringstream content_stream;
    
    std::string hostname = platf::get_host_name();
    std::string local_ip = get_local_ip();
    std::string formatted_ip = format_ip_address(local_ip);
    // 构建纯文本内容
    content_stream << (is_chinese ? "Sunshine系统通知" : "Sunshine System Notification") << "\n";
    content_stream << "================================\n";
    content_stream << (is_chinese ? "事件: " : "Event: ") << get_event_title(event.type, is_chinese) << "\n";
    content_stream << (is_chinese ? "主机名: " : "Hostname: ") << hostname << "\n";
    
    if (!formatted_ip.empty()) {
      content_stream << (is_chinese ? "IP地址: " : "IP Address: ") << formatted_ip << "\n";
    }
    // 添加事件特定信息
    switch (event.type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
      case event_type_t::CONFIG_PIN_FAILED: {
        if (!event.client_name.empty()) {
          content_stream << (is_chinese ? "客户端名称: " : "Client Name: ") << event.client_name << "\n";
        }
        if (!event.client_ip.empty()) {
          content_stream << (is_chinese ? "客户端IP: " : "Client IP: ") << event.client_ip << "\n";
        }
        if (!event.server_ip.empty()) {
          content_stream << (is_chinese ? "服务器IP: " : "Server IP: ") << event.server_ip << "\n";
        }
        break;
      }
      case event_type_t::NV_APP_LAUNCH:
      case event_type_t::NV_APP_RESUME:
      case event_type_t::NV_APP_TERMINATE: {
        if (!event.app_name.empty()) {
          content_stream << (is_chinese ? "应用名称: " : "App Name: ") << event.app_name << "\n";
        }
        if (event.app_id > 0) {
          content_stream << (is_chinese ? "应用ID: " : "App ID: ") << event.app_id << "\n";
        }
        if (!event.client_name.empty()) {
          content_stream << (is_chinese ? "客户端: " : "Client: ") << event.client_name << "\n";
        }
        if (!event.client_ip.empty()) {
          content_stream << (is_chinese ? "客户端IP: " : "Client IP: ") << event.client_ip << "\n";
        }
        if (!event.server_ip.empty()) {
          content_stream << (is_chinese ? "服务器IP: " : "Server IP: ") << event.server_ip << "\n";
        }
        break;
      }
      case event_type_t::NV_SESSION_START:
      case event_type_t::NV_SESSION_END: {
        if (!event.app_name.empty()) {
          content_stream << (is_chinese ? "应用名称: " : "App Name: ") << event.app_name << "\n";
        }
        if (!event.client_name.empty()) {
          content_stream << (is_chinese ? "客户端: " : "Client: ") << event.client_name << "\n";
        }
        if (!event.session_id.empty()) {
          content_stream << (is_chinese ? "会话ID: " : "Session ID: ") << event.session_id << "\n";
        }
        break;
      }
      default:
        break;
    }
    content_stream << (is_chinese ? "时间: " : "Time: ") << format_timestamp(event.timestamp) << "\n";
    // 添加错误信息
    auto error_it = event.extra_data.find("error");
    if (error_it != event.extra_data.end()) {
      content_stream << (is_chinese ? "错误信息: " : "Error: ") << error_it->second << "\n";
    }
    return content_stream.str();
  }

  std::string WebhookFormat::generate_json_content(const event_t& event, bool is_chinese) const
  {
    std::ostringstream json_stream;
    std::string hostname = platf::get_host_name();
    std::string local_ip = get_local_ip();
    std::string formatted_ip = format_ip_address(local_ip);
    json_stream << "{";
    json_stream << "\"system\":\"Sunshine\",";
    json_stream << "\"hostname\":\"" << hostname << "\",";
    if (!formatted_ip.empty()) {
      json_stream << "\"ip_address\":\"" << formatted_ip << "\",";
    }
    json_stream << "\"event_type\":\"" << get_event_title(event.type, is_chinese) << "\",";
    json_stream << "\"timestamp\":\"" << format_timestamp(event.timestamp) << "\"";
    
    // 添加事件特定字段
    if (!event.client_name.empty()) {
      json_stream << ",\"client_name\":\"" << event.client_name << "\"";
    }
    if (!event.client_ip.empty()) {
      json_stream << ",\"client_ip\":\"" << event.client_ip << "\"";
    }
    if (!event.server_ip.empty()) {
      json_stream << ",\"server_ip\":\"" << event.server_ip << "\"";
    }
    if (!event.app_name.empty()) {
      json_stream << ",\"app_name\":\"" << event.app_name << "\"";
    }
    if (event.app_id > 0) {
      json_stream << ",\"app_id\":" << event.app_id;
    }
    if (!event.session_id.empty()) {
      json_stream << ",\"session_id\":\"" << event.session_id << "\"";
    }
    
    // 添加额外数据
    if (!event.extra_data.empty()) {
      json_stream << ",\"extra_data\":{";
      bool first = true;
      for (const auto& [key, value] : event.extra_data) {
        if (!first) json_stream << ",";
        json_stream << "\"" << key << "\":\"" << value << "\"";
        first = false;
      }
      json_stream << "}";
    }
    
    json_stream << "}";
    return json_stream.str();
  }

  std::string WebhookFormat::generate_custom_content(const event_t& event, bool is_chinese) const
  {
    auto it = custom_templates_.find(event.type);
    if (it != custom_templates_.end()) {
      return replace_template_variables(it->second, event, is_chinese);
    }
    
    // 如果没有自定义模板，回退到Markdown格式
    return generate_markdown_content(event, is_chinese);
  }

  std::string WebhookFormat::replace_template_variables(const std::string& template_str, const event_t& event, bool is_chinese) const
  {
    std::string result = template_str;
    
    // 替换变量
    std::string hostname = platf::get_host_name();
    std::string local_ip = get_local_ip();
    std::string formatted_ip = format_ip_address(local_ip);
    
    // 使用正则表达式替换变量
    result = std::regex_replace(result, std::regex("\\{\\{hostname\\}\\}"), hostname);
    result = std::regex_replace(result, std::regex("\\{\\{ip_address\\}\\}"), formatted_ip);
    result = std::regex_replace(result, std::regex("\\{\\{event_title\\}\\}"), get_event_title(event.type, is_chinese));
    result = std::regex_replace(result, std::regex("\\{\\{timestamp\\}\\}"), format_timestamp(event.timestamp));
    result = std::regex_replace(result, std::regex("\\{\\{client_name\\}\\}"), event.client_name);
    result = std::regex_replace(result, std::regex("\\{\\{client_ip\\}\\}"), event.client_ip);
    result = std::regex_replace(result, std::regex("\\{\\{server_ip\\}\\}"), event.server_ip);
    result = std::regex_replace(result, std::regex("\\{\\{app_name\\}\\}"), event.app_name);
    result = std::regex_replace(result, std::regex("\\{\\{app_id\\}\\}"), std::to_string(event.app_id));
    result = std::regex_replace(result, std::regex("\\{\\{session_id\\}\\}"), event.session_id);
    
    return result;
  }

  std::string WebhookFormat::generate_content(const event_t& event, bool is_chinese) const
  {
    switch (format_type_) {
      case format_type_t::MARKDOWN:
        return generate_markdown_content(event, is_chinese);
      case format_type_t::TEXT:
        return generate_text_content(event, is_chinese);
      case format_type_t::JSON:
        return generate_json_content(event, is_chinese);
      case format_type_t::CUSTOM:
        return generate_custom_content(event, is_chinese);
      default:
        return generate_markdown_content(event, is_chinese);
    }
  }

  std::string WebhookFormat::generate_json_payload(const event_t& event, bool is_chinese) const
  {
    std::string content = generate_content(event, is_chinese);
    
    // 检查内容长度限制（限制4096字节）
    const size_t MAX_CONTENT_LENGTH = 4096;
    if (content.length() > MAX_CONTENT_LENGTH) {
      // 截断内容并添加省略号
      content = content.substr(0, MAX_CONTENT_LENGTH - 10) + "...";
      BOOST_LOG(warning) << "Webhook content truncated to " << MAX_CONTENT_LENGTH << " bytes";
    }
    
    switch (format_type_) {
      case format_type_t::MARKDOWN:
        return "{\"msgtype\":\"markdown\",\"markdown\":{\"content\":\"" + sanitize_json_string(content) + "\"}}";
      case format_type_t::TEXT:
        return "{\"msgtype\":\"text\",\"text\":{\"content\":\"" + sanitize_json_string(content) + "\"}}";
      case format_type_t::JSON:
        return content; // JSON格式直接返回内容
      case format_type_t::CUSTOM:
        return "{\"msgtype\":\"markdown\",\"markdown\":{\"content\":\"" + sanitize_json_string(content) + "\"}}";
      default:
        return "{\"msgtype\":\"markdown\",\"markdown\":{\"content\":\"" + sanitize_json_string(content) + "\"}}";
    }
  }

  void init_webhook_format()
  {
    // 初始化默认格式配置
    g_webhook_format.set_format_type(format_type_t::MARKDOWN);
    g_webhook_format.set_use_colors(true);
    g_webhook_format.set_simplify_ip(true);
    g_webhook_format.set_time_format("%Y-%m-%d %H:%M:%S");
  }

  void load_format_config()
  {
    // 从配置文件加载格式设置
    // 这里可以添加从config::webhook读取格式配置的逻辑
    init_webhook_format();
  }

  void configure_webhook_format(bool use_markdown)
  {
    if (use_markdown) {
      g_webhook_format.set_format_type(format_type_t::MARKDOWN);
    } else {
      g_webhook_format.set_format_type(format_type_t::TEXT);
    }
    
    // webhook优化设置
    g_webhook_format.set_use_colors(true);      // 启用颜色支持
    g_webhook_format.set_simplify_ip(true);     // 简化IP显示
    g_webhook_format.set_time_format("%Y-%m-%d %H:%M:%S"); // 标准时间格式
    
    BOOST_LOG(debug) << "Webhook configured (Markdown: " << use_markdown << ")";
  }

  bool validate_webhook_content_length(const std::string& content) {
    const size_t MAX_CONTENT_LENGTH = 4096;
    return content.length() <= MAX_CONTENT_LENGTH;
  }

} // namespace webhook
