/**
 * @file src/webhook/webhook_format.cpp
 * @brief Webhook payload formatting.
 */
#include "webhook_format.h"

#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "src/logging.h"
#include "src/platform/common.h"
#include "webhook.h"

namespace webhook {
  namespace {
    constexpr std::size_t MAX_CONTENT_LENGTH = 4096;
    constexpr std::size_t MAX_JSON_FIELD_LENGTH = 512;

    std::string truncate_utf8(std::string value, std::size_t maximum) {
      if (value.size() <= maximum) {
        return value;
      }
      if (maximum <= 3) {
        return std::string(maximum, '.');
      }

      std::size_t cut = maximum - 3;
      while (cut > 0 && cut < value.size() &&
             (static_cast<unsigned char>(value[cut]) & 0xc0U) == 0x80U) {
        --cut;
      }
      value.resize(cut);
      value += "...";
      return value;
    }

    std::string bounded_json_value(const std::string &value) {
      return truncate_utf8(value, MAX_JSON_FIELD_LENGTH);
    }

    std::string escape_markup(const std::string &value) {
      std::string escaped;
      escaped.reserve(value.size());
      for (const char c : value) {
        switch (c) {
          case '&':
            escaped += "&amp;";
            break;
          case '<':
            escaped += "&lt;";
            break;
          case '>':
            escaped += "&gt;";
            break;
          case '"':
            escaped += "&quot;";
            break;
          case '\\':
          case '`':
          case '*':
          case '_':
          case '[':
          case ']':
          case '!':
          case '~':
            escaped += '\\';
            escaped += c;
            break;
          case '\r':
          case '\n':
            escaped += ' ';
            break;
          default:
            escaped += c;
            break;
        }
      }
      return escaped;
    }

    void replace_all(std::string &value, const std::string &token, const std::string &replacement) {
      std::size_t position = 0;
      while ((position = value.find(token, position)) != std::string::npos) {
        value.replace(position, token.size(), replacement);
        position += replacement.size();
      }
    }

    std::string dump_utf8(const nlohmann::json &value) {
      return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    nlohmann::json make_message_payload(
      format_type_t format_type,
      std::string content,
      int event_id,
      const std::string &event_type
    ) {
      nlohmann::json payload;
      if (format_type == format_type_t::TEXT) {
        payload = {{"msgtype", "text"}, {"text", {{"content", std::move(content)}}}};
      }
      else {
        payload = {{"msgtype", "markdown"}, {"markdown", {{"content", std::move(content)}}}};
      }
      payload["event_id"] = event_id;
      payload["event_type"] = event_type;
      return payload;
    }
  }  // namespace

  WebhookFormat g_webhook_format;

  WebhookFormat::WebhookFormat(format_type_t format_type):
      format_type_(format_type),
      use_colors_(true),
      simplify_ip_(true) {
  }

  void WebhookFormat::set_format_type(format_type_t format_type) {
    format_type_ = format_type;
  }

  format_type_t WebhookFormat::get_format_type() const {
    return format_type_;
  }

  void WebhookFormat::set_custom_template(event_type_t event_type, const std::string &template_str) {
    custom_templates_[event_type] = template_str;
  }

  void WebhookFormat::set_use_colors(bool use_colors) {
    use_colors_ = use_colors;
  }

  void WebhookFormat::set_simplify_ip(bool simplify_ip) {
    simplify_ip_ = simplify_ip;
  }

  std::string WebhookFormat::format_ip_address(const std::string &ip) const {
    if (ip.empty() || !simplify_ip_) {
      return ip;
    }
    if (ip.find(':') == std::string::npos) {
      return ip;
    }
    if (ip == "::1") {
      return "IPv6 (loopback)";
    }
    if (ip.rfind("fe80:", 0) == 0) {
      return "IPv6 (link-local)";
    }
    return "IPv6";
  }

  std::string WebhookFormat::format_timestamp(const std::string &timestamp) const {
    return timestamp;
  }

  std::string WebhookFormat::get_event_color(event_type_t event_type) const {
    if (!use_colors_) {
      return {};
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
      default:
        return colors::COLOR_COMMENT;
    }
  }

  std::string WebhookFormat::get_event_title(event_type_t event_type, bool is_chinese) const {
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

  std::string WebhookFormat::generate_markdown_content(const event_t &event, bool is_chinese) const {
    std::ostringstream content;
    const auto field = [&content, is_chinese](const char *zh_label, const char *en_label, const std::string &value) {
      if (!value.empty()) {
        content << '>' << (is_chinese ? zh_label : en_label)
                << ": <font color=\"comment\">" << escape_markup(value) << "</font>\n";
      }
    };

    content << (is_chinese ? "**Sunshine 系统通知**" : "**Sunshine System Notification**") << "\n\n";
    const auto title = get_event_title(event.type, is_chinese);
    const auto color = get_event_color(event.type);
    if (use_colors_ && !color.empty()) {
      content << "<font color=\"" << color << "\">**" << title << "**</font>\n\n";
    }
    else {
      content << "**" << title << "**\n\n";
    }

    field("主机名", "Hostname", platf::get_host_name());
    field("服务端 IP", "Server IP", format_ip_address(event.server_ip));

    switch (event.type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
      case event_type_t::CONFIG_PIN_FAILED:
        field("客户端名称", "Client Name", event.client_name);
        field("客户端 IP", "Client IP", event.client_ip);
        break;
      case event_type_t::NV_APP_LAUNCH:
      case event_type_t::NV_APP_RESUME:
      case event_type_t::NV_APP_TERMINATE: {
        field("应用名称", "App Name", event.app_name);
        if (event.app_id > 0) {
          field("应用 ID", "App ID", std::to_string(event.app_id));
        }
        field("客户端", "Client", event.client_name);
        field("客户端 IP", "Client IP", event.client_ip);
        const auto resolution = event.extra_data.find("resolution");
        const auto fps = event.extra_data.find("fps");
        const auto audio = event.extra_data.find("host_audio");
        if (resolution != event.extra_data.end()) {
          field("分辨率", "Resolution", resolution->second);
        }
        if (fps != event.extra_data.end()) {
          field("帧率", "FPS", fps->second);
        }
        if (audio != event.extra_data.end()) {
          const bool enabled = audio->second == "true";
          field("音频", "Audio", enabled ? (is_chinese ? "启用" : "Enabled") : (is_chinese ? "禁用" : "Disabled"));
        }
        break;
      }
      case event_type_t::NV_SESSION_START:
      case event_type_t::NV_SESSION_END: {
        field("应用名称", "App Name", event.app_name);
        field("客户端", "Client", event.client_name);
        field("客户端 IP", "Client IP", event.client_ip);
        field("会话 ID", "Session ID", event.session_id);
        const auto resolution = event.extra_data.find("resolution");
        const auto fps = event.extra_data.find("fps");
        const auto reason = event.extra_data.find("reason");
        if (resolution != event.extra_data.end()) {
          field("分辨率", "Resolution", resolution->second);
        }
        if (fps != event.extra_data.end()) {
          field("帧率", "FPS", fps->second);
        }
        if (reason != event.extra_data.end()) {
          field("结束原因", "End Reason", reason->second);
        }
        break;
      }
      default:
        break;
    }

    field("时间", "Time", format_timestamp(event.timestamp));
    const auto error = event.extra_data.find("error");
    if (error != event.extra_data.end()) {
      content << '>' << (is_chinese ? "错误信息" : "Error")
              << ": <font color=\"warning\">" << escape_markup(error->second) << "</font>";
    }
    return content.str();
  }

  std::string WebhookFormat::generate_text_content(const event_t &event, bool is_chinese) const {
    std::ostringstream content;
    const auto field = [&content, is_chinese](const char *zh_label, const char *en_label, const std::string &value) {
      if (!value.empty()) {
        content << (is_chinese ? zh_label : en_label) << ": " << value << '\n';
      }
    };

    content << (is_chinese ? "Sunshine 系统通知" : "Sunshine System Notification") << '\n';
    field("事件", "Event", get_event_title(event.type, is_chinese));
    field("主机名", "Hostname", platf::get_host_name());
    field("服务端 IP", "Server IP", format_ip_address(event.server_ip));
    field("客户端名称", "Client Name", event.client_name);
    field("客户端 IP", "Client IP", event.client_ip);
    field("应用名称", "App Name", event.app_name);
    if (event.app_id > 0) {
      field("应用 ID", "App ID", std::to_string(event.app_id));
    }
    field("会话 ID", "Session ID", event.session_id);
    field("时间", "Time", format_timestamp(event.timestamp));
    return content.str();
  }

  std::string WebhookFormat::generate_json_content(const event_t &event, bool is_chinese) const {
    nlohmann::json body {
      {"system", "Sunshine"},
      {"hostname", bounded_json_value(platf::get_host_name())},
      {"event_id", event_type_id(event.type)},
      {"event_type", event_type_name(event.type)},
      {"event_title", get_event_title(event.type, is_chinese)},
      {"timestamp", bounded_json_value(format_timestamp(event.timestamp))}
    };
    if (!event.client_name.empty()) body["client_name"] = bounded_json_value(event.client_name);
    if (!event.client_ip.empty()) body["client_ip"] = bounded_json_value(event.client_ip);
    if (!event.server_ip.empty()) body["server_ip"] = bounded_json_value(event.server_ip);
    if (!event.app_name.empty()) body["app_name"] = bounded_json_value(event.app_name);
    if (event.app_id > 0) body["app_id"] = event.app_id;
    if (!event.session_id.empty()) body["session_id"] = bounded_json_value(event.session_id);

    for (const auto &[key, value] : event.extra_data) {
      body["extra_data"][bounded_json_value(key)] = bounded_json_value(value);
    }

    auto serialized = dump_utf8(body);
    if (serialized.size() > MAX_CONTENT_LENGTH) {
      body.erase("extra_data");
      body["truncated"] = true;
      serialized = dump_utf8(body);
    }
    if (serialized.size() > MAX_CONTENT_LENGTH) {
      body = {
        {"system", "Sunshine"},
        {"event_id", event_type_id(event.type)},
        {"event_type", event_type_name(event.type)},
        {"event_title", get_event_title(event.type, is_chinese)},
        {"timestamp", bounded_json_value(format_timestamp(event.timestamp))},
        {"truncated", true}
      };
      serialized = dump_utf8(body);
    }
    return serialized;
  }

  std::string WebhookFormat::generate_custom_content(const event_t &event, bool is_chinese) const {
    const auto custom = custom_templates_.find(event.type);
    if (custom == custom_templates_.end()) {
      return generate_markdown_content(event, is_chinese);
    }
    return replace_template_variables(custom->second, event, is_chinese);
  }

  std::string WebhookFormat::replace_template_variables(const std::string &template_str, const event_t &event, bool is_chinese) const {
    std::string result = template_str;
    replace_all(result, "{{hostname}}", escape_markup(platf::get_host_name()));
    replace_all(result, "{{ip_address}}", escape_markup(format_ip_address(event.server_ip)));
    replace_all(result, "{{event_title}}", get_event_title(event.type, is_chinese));
    replace_all(result, "{{timestamp}}", escape_markup(format_timestamp(event.timestamp)));
    replace_all(result, "{{client_name}}", escape_markup(event.client_name));
    replace_all(result, "{{client_ip}}", escape_markup(event.client_ip));
    replace_all(result, "{{server_ip}}", escape_markup(event.server_ip));
    replace_all(result, "{{app_name}}", escape_markup(event.app_name));
    replace_all(result, "{{app_id}}", std::to_string(event.app_id));
    replace_all(result, "{{session_id}}", escape_markup(event.session_id));
    const auto error = event.extra_data.find("error");
    replace_all(result, "{{error}}", error == event.extra_data.end() ? std::string {} : escape_markup(error->second));
    return result;
  }

  std::string WebhookFormat::generate_content(const event_t &event, bool is_chinese) const {
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

  std::string WebhookFormat::generate_json_payload(const event_t &event, bool is_chinese) const {
    std::string content = generate_content(event, is_chinese);
    if (format_type_ == format_type_t::JSON) {
      return content;
    }

    if (content.size() > MAX_CONTENT_LENGTH) {
      content = truncate_utf8(std::move(content), MAX_CONTENT_LENGTH);
      BOOST_LOG(warning) << "Webhook content was truncated to " << MAX_CONTENT_LENGTH << " bytes";
    }

    return dump_utf8(make_message_payload(
      format_type_,
      std::move(content),
      event_type_id(event.type),
      event_type_name(event.type)
    ));
  }

  std::string WebhookFormat::generate_test_json_payload(bool is_chinese) const {
    const auto hostname = bounded_json_value(platf::get_host_name());
    const auto timestamp = get_current_timestamp();
    const auto heading = is_chinese ? "Sunshine Webhook 测试" : "Sunshine Webhook Test";
    const auto title = is_chinese ? "测试通知" : "Test Notification";
    const auto event_title = is_chinese ? "Webhook 测试" : "Webhook Test";
    const auto result_label = is_chinese ? "结果" : "Result";
    const auto result = is_chinese ? "Webhook 接收地址已收到测试请求" : "Webhook endpoint reached";
    const auto hostname_label = is_chinese ? "主机名" : "Hostname";
    const auto event_type_label = is_chinese ? "事件类型" : "Event Type";
    const auto sample_application_label = is_chinese ? "示例应用" : "Sample Application";
    const auto sample_application = is_chinese ? "Sunshine 测试应用" : "Sunshine Test Application";
    const auto sample_client_label = is_chinese ? "示例客户端" : "Sample Client";
    const auto sample_client = is_chinese ? "Sunshine 测试客户端" : "Sunshine Test Client";
    const auto sample_stream_label = is_chinese ? "示例串流" : "Sample Stream";
    const auto sample_stream = is_chinese ? "1920x1080，60 FPS，音频已启用" : "1920x1080, 60 FPS, Audio Enabled";
    const auto time_label = is_chinese ? "时间" : "Time";
    if (format_type_ == format_type_t::JSON) {
      return dump_utf8({
        {"event_id", -1},
        {"event_type", "webhook_test"},
        {"event_title", event_title},
        {"system", "Sunshine"},
        {"hostname", hostname},
        {"timestamp", timestamp},
        {"result", result},
        {"sample", {
          {"app_name", sample_application},
          {"client_name", sample_client},
          {"resolution", "1920x1080"},
          {"fps", 60},
          {"audio", is_chinese ? "已启用" : "Enabled"}
        }}
      });
    }

    std::ostringstream content;
    if (format_type_ == format_type_t::TEXT) {
      content << heading << "\n\n"
              << result_label << ": " << result << '\n'
              << hostname_label << ": " << hostname << '\n'
              << event_type_label << ": webhook_test\n"
              << sample_application_label << ": " << sample_application << '\n'
              << sample_client_label << ": " << sample_client << '\n'
              << sample_stream_label << ": " << sample_stream << '\n'
              << time_label << ": " << timestamp << '\n';
    }
    else {
      content << "**" << heading << "**\n\n"
              << "<font color=\"info\">**" << title << "**</font>\n\n"
              << '>' << result_label << ": <font color=\"comment\">" << result << "</font>\n"
              << '>' << hostname_label << ": <font color=\"comment\">" << escape_markup(hostname) << "</font>\n"
              << '>' << event_type_label << ": <font color=\"comment\">webhook_test</font>\n"
              << '>' << sample_application_label << ": <font color=\"comment\">" << sample_application << "</font>\n"
              << '>' << sample_client_label << ": <font color=\"comment\">" << sample_client << "</font>\n"
              << '>' << sample_stream_label << ": <font color=\"comment\">" << sample_stream << "</font>\n"
              << '>' << time_label << ": <font color=\"comment\">" << escape_markup(timestamp) << "</font>\n";
    }
    return dump_utf8(make_message_payload(
      format_type_,
      content.str(),
      -1,
      "webhook_test"
    ));
  }

  void init_webhook_format() {
    g_webhook_format.set_format_type(format_type_t::MARKDOWN);
    g_webhook_format.set_use_colors(true);
    g_webhook_format.set_simplify_ip(true);
  }

  void configure_webhook_format(bool use_markdown) {
    init_webhook_format();
    if (!use_markdown) {
      g_webhook_format.set_format_type(format_type_t::TEXT);
    }
    BOOST_LOG(debug) << "Webhook configured (Markdown: " << use_markdown << ')';
  }

  bool validate_webhook_content_length(const std::string &content) {
    return content.size() <= MAX_CONTENT_LENGTH;
  }

}  // namespace webhook
