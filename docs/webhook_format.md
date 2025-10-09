# Sunshine Webhook格式配置

## 概述

Sunshine支持多种webhook格式，包括Markdown、纯文本和JSON格式，暂时没有适配的对象平台。

## Webhook格式规范

### 消息格式要求
- **msgtype**: 消息类型，支持 `markdown`、`text`、`json`
- **content**: 内容，最长不超过4096个字节，必须是utf8编码

### 支持的Markdown语法
- 支持标准Markdown语法
- 支持HTML标签（如 `<font color="warning">`）
- 支持引用块（`>`）
- 支持粗体（`**text**`）

## 配置方法

### 1. 自动配置（默认）
```cpp
// 系统会自动配置为Markdown格式
configure_webhook_format(true);  // 使用Markdown格式
```

### 2. 手动配置
```cpp
// 配置为Markdown格式（推荐）
g_webhook_format.set_format_type(format_type_t::MARKDOWN);
g_webhook_format.set_use_colors(true);
g_webhook_format.set_simplify_ip(true);

// 或者配置为纯文本格式
configure_webhook_format(false);
```

## 输出格式示例

### 配置配对失败通知
```json
{
    "msgtype": "markdown",
    "markdown": {
        "content": "**Sunshine系统通知**\n\n<font color=\"warning\">**配置配对失败**</font>\n\n>主机名:<font color=\"comment\">sunshine</font>\n>IP地址:<font color=\"comment\">IPv6 (本地链路)</font>\n>客户端名称:<font color=\"comment\">1111</font>\n>时间:<font color=\"comment\">2025-10-07 16:36:33</font>\n>错误信息:<font color=\"warning\">PIN码验证失败</font>"
    }
}
```

### 应用启动通知
```json
{
    "msgtype": "markdown",
    "markdown": {
        "content": "**Sunshine系统通知**\n\n<font color=\"info\">**应用启动**</font>\n\n>主机名:<font color=\"comment\">sunshine</font>\n>IP地址:<font color=\"comment\">192.168.1.100</font>\n>应用名称:<font color=\"comment\">Steam</font>\n>应用ID:<font color=\"comment\">12345</font>\n>客户端:<font color=\"comment\">Moonlight</font>\n>客户端IP:<font color=\"comment\">192.168.1.50</font>\n>分辨率:<font color=\"comment\">1920x1080</font>\n>帧率:<font color=\"comment\">60</font>\n>音频:<font color=\"comment\">启用</font>\n>时间:<font color=\"comment\">2025-10-07 16:36:33</font>"
    }
}
```

## 颜色规范

Sunshine webhook支持以下颜色：
- `info` - 信息（绿色）
- `warning` - 警告（橙色）
- `error` - 错误（红色）
- `comment` - 注释（灰色）

## 内容长度限制

- 最大长度：4096字节
- 自动截断：超过限制时自动截断并添加省略号
- 日志记录：截断时会记录警告日志

## 自定义模板

### 设置自定义模板
```cpp
g_webhook_format.set_format_type(format_type_t::CUSTOM);
g_webhook_format.set_custom_template(
    event_type_t::CONFIG_PIN_FAILED,
    "🚨 **配对失败通知**\n\n"
    "**主机:** {{hostname}}\n"
    "**IP:** {{ip_address}}\n"
    "**客户端:** {{client_name}}\n"
    "**时间:** {{timestamp}}\n"
    "**错误:** {{error}}"
);
```

### 支持的模板变量
- `{{hostname}}` - 主机名
- `{{ip_address}}` - IP地址
- `{{event_title}}` - 事件标题
- `{{timestamp}}` - 时间戳
- `{{client_name}}` - 客户端名称
- `{{client_ip}}` - 客户端IP
- `{{app_name}}` - 应用名称
- `{{app_id}}` - 应用ID
- `{{session_id}}` - 会话ID

## 验证函数

### 检查内容长度
```cpp
std::string content = generate_content(event, is_chinese);
if (validate_webhook_content_length(content)) {
    // 内容长度符合要求
} else {
    // 内容过长，需要截断
}
```

## 最佳实践

1. **使用Markdown格式** - 提供更好的视觉效果
2. **启用颜色** - 使用颜色区分不同类型的事件
3. **简化IP显示** - 避免显示复杂的IPv6地址
4. **控制内容长度** - 避免超过4096字节限制
5. **使用引用块** - 提高信息层次感

## 故障排除

### 常见问题
1. **内容过长** - 检查是否超过4096字节限制
2. **格式错误** - 确保JSON格式正确
3. **编码问题** - 确保使用UTF-8编码
4. **颜色不显示** - 检查接收端是否支持HTML标签

### 调试方法
```cpp
// 启用调试日志
BOOST_LOG(debug) << "Webhook content: " << content;
BOOST_LOG(debug) << "Content length: " << content.length();
```
