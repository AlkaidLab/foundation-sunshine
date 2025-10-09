/**
 * @file src/webhook_format.h
 * @brief Webhook格式配置和模板定义
 */
#pragma once

#include <string>
#include <map>
#include <functional>
#include "webhook.h"

namespace webhook {

  /**
   * @brief Webhook格式类型
   */
  enum class format_type_t {
    MARKDOWN,     // Markdown格式（支持HTML标签）
    TEXT,         // 纯文本格式
    JSON,         // JSON格式
    CUSTOM        // 自定义格式
  };

  /**
   * @brief 颜色类型定义
   */
  namespace colors {
    constexpr const char* COLOR_INFO = "info";           // 信息（绿色）
    constexpr const char* COLOR_WARNING = "warning";     // 警告（橙色）
    constexpr const char* COLOR_ERROR = "error";         // 错误（红色）
    constexpr const char* COLOR_COMMENT = "comment";     // 注释（灰色）
    constexpr const char* COLOR_SUCCESS = "success";     // 成功（蓝色）
  }

  /**
   * @brief Webhook格式配置类
   */
  class WebhookFormat {
  public:
    /**
     * @brief 构造函数
     * @param format_type 格式类型
     */
    explicit WebhookFormat(format_type_t format_type = format_type_t::MARKDOWN);

    /**
     * @brief 设置格式类型
     * @param format_type 格式类型
     */
    void set_format_type(format_type_t format_type);

    /**
     * @brief 获取格式类型
     * @return 格式类型
     */
    format_type_t get_format_type() const;

    /**
     * @brief 设置自定义模板
     * @param event_type 事件类型
     * @param template_str 模板字符串
     */
    void set_custom_template(event_type_t event_type, const std::string& template_str);

    /**
     * @brief 设置是否使用颜色
     * @param use_colors 是否使用颜色
     */
    void set_use_colors(bool use_colors);

    /**
     * @brief 设置是否简化IP显示
     * @param simplify_ip 是否简化IP显示
     */
    void set_simplify_ip(bool simplify_ip);

    /**
     * @brief 设置时间格式
     * @param time_format 时间格式字符串
     */
    void set_time_format(const std::string& time_format);

    /**
     * @brief 生成webhook内容
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return 格式化的内容字符串
     */
    std::string generate_content(const event_t& event, bool is_chinese) const;

    /**
     * @brief 生成完整的JSON payload
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return JSON字符串
     */
    std::string generate_json_payload(const event_t& event, bool is_chinese) const;

  private:
    format_type_t format_type_;
    bool use_colors_;
    bool simplify_ip_;
    std::string time_format_;
    std::map<event_type_t, std::string> custom_templates_;

    /**
     * @brief 格式化IP地址
     * @param ip IP地址字符串
     * @return 格式化后的IP地址
     */
    std::string format_ip_address(const std::string& ip) const;

    /**
     * @brief 格式化时间戳
     * @param timestamp ISO 8601格式时间戳
     * @return 格式化后的时间字符串
     */
    std::string format_timestamp(const std::string& timestamp) const;

    /**
     * @brief 获取事件颜色
     * @param event_type 事件类型
     * @return 颜色字符串
     */
    std::string get_event_color(event_type_t event_type) const;

    /**
     * @brief 获取事件标题
     * @param event_type 事件类型
     * @param is_chinese 是否使用中文
     * @return 事件标题
     */
    std::string get_event_title(event_type_t event_type, bool is_chinese) const;

    /**
     * @brief 生成Markdown格式内容
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return Markdown内容
     */
    std::string generate_markdown_content(const event_t& event, bool is_chinese) const;

    /**
     * @brief 生成文本格式内容
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return 文本内容
     */
    std::string generate_text_content(const event_t& event, bool is_chinese) const;

    /**
     * @brief 生成JSON格式内容
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return JSON内容
     */
    std::string generate_json_content(const event_t& event, bool is_chinese) const;

    /**
     * @brief 生成自定义格式内容
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return 自定义内容
     */
    std::string generate_custom_content(const event_t& event, bool is_chinese) const;

    /**
     * @brief 替换模板变量
     * @param template_str 模板字符串
     * @param event 事件数据
     * @param is_chinese 是否使用中文
     * @return 替换后的字符串
     */
    std::string replace_template_variables(const std::string& template_str, 
                                          const event_t& event, 
                                          bool is_chinese) const;
  };

  /**
   * @brief 全局webhook格式实例
   */
  extern WebhookFormat g_webhook_format;

  /**
   * @brief 初始化webhook格式配置
   */
  void init_webhook_format();

  /**
   * @brief 从配置文件加载格式设置
   */
  void load_format_config();

  /**
   * @brief 配置格式
   * @param use_markdown 是否使用Markdown格式（默认true）
   */
  void configure_webhook_format(bool use_markdown = true);

  /**
   * @brief 验证内容长度是否符合webhook要求
   * @param content 内容字符串
   * @return 是否符合长度要求
   */
  bool validate_webhook_content_length(const std::string& content);

} // namespace webhook
