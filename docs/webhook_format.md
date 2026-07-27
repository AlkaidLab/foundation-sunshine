# Sunshine Webhook 格式

## 配置位置

Webhook 的唯一有效配置位于所选 `sunshine.conf` 同目录下的
`webhook_auth.json`。默认布局为 `config/webhook_auth.json`，不是
`credentials/webhook_auth.json`。

```json
{
  "webhook_enabled": false,
  "webhook_events": "0,1,2,3,4,5,6",
  "webhook_skip_ssl_verify": false,
  "webhook_timeout": 5000,
  "webhook_url": ""
}
```

- `webhook_timeout` 单位为毫秒，范围为 1000–15000，默认 5000。
- `webhook_events` 是逗号分隔的稳定数字编号；默认全选，`-1` 表示不发送任何生产事件。
- 文件缺失表示关闭 Webhook。
- 文件损坏只会禁用 Webhook，不影响 Sunshine 主程序启动。
- 更新现有配置时会把旧文件的原始内容备份为 `webhook_auth.json.bak`；该文件只供人工恢复，不会被自动读取。
- `sunshine.conf` 中残留的同名旧字段不读取、不迁移、不回退；它们只按普通未知字段随 `/api/config` 返回和保存。
- Web UI 通过认证后的 `/api/webhook/config` 独立读写；测试使用 `/api/webhook/test`。

## URL 与 TLS

- 支持 HTTP 和 HTTPS，推荐使用 HTTPS。
- URL 必须包含主机，不支持 URL 中嵌入用户名或密码。
- HTTPS 默认校验证书链和服务器身份。
- Windows 版在开启校验时把本机和当前用户的系统 ROOT 证书导入 Webhook
  独立的 OpenSSL 上下文，不依赖打包环境中是否存在 OpenSSL 默认 CA 文件。
- 该实现使用 Windows ROOT 作为 OpenSSL 信任锚，不调用 Windows 原生证书链
  策略；当前目标是让常规可信证书可用，同时保留用户显式跳过校验的选择。
- 系统证书存储不可用时按 HTTPS 传输失败处理，不会自动关闭证书校验。
- `webhook_skip_ssl_verify=true` 只适用于用户明确接受风险的自签名或测试环境。
- 不自动跟随重定向。

## 事件编号

| 编号 | `event_type` | 事件 |
|---:|---|---|
| 0 | `config_pair_success` | 配对成功 |
| 1 | `config_pair_failed` | 配对失败 |
| 2 | `nv_app_launch` | 应用启动 |
| 3 | `nv_app_resume` | 应用恢复 |
| 4 | `nv_app_terminate` | 应用终止 |
| 5 | `nv_session_start` | 会话开始 |
| 6 | `nv_session_end` | 会话结束 |

这些编号属于持久化和接收端协议，后续不能因枚举代码顺序调整而改变。

## Payload

生产事件默认使用 Markdown 结构，并在顶层提供稳定事件标识。以下是应用启动
事件的当前上报样例；示例值均为虚构值，实际 JSON 在网络上传输时为单行紧凑格式：

```json
{
  "event_id": 2,
  "event_type": "nv_app_launch",
  "markdown": {
    "content": "**Sunshine System Notification**\n\n<font color=\"info\">**Application Launched**</font>\n\n>Hostname: <font color=\"comment\">sunshine-host</font>\n>Server IP: <font color=\"comment\">192.168.1.10</font>\n>App Name: <font color=\"comment\">Example App</font>\n>App ID: <font color=\"comment\">123</font>\n>Client: <font color=\"comment\">Moonlight Client</font>\n>Client IP: <font color=\"comment\">192.168.1.20</font>\n>Resolution: <font color=\"comment\">1920x1080</font>\n>FPS: <font color=\"comment\">60</font>\n>Audio: <font color=\"comment\">Enabled</font>\n>Time: <font color=\"comment\">2026-07-27 09:20:29.979</font>\n"
  },
  "msgtype": "markdown"
}
```

不同事件只改变 `event_id`、`event_type` 和 `markdown.content` 中实际存在的字段。
空字段不会输出。应用终止事件当前只包含主机、应用、应用 ID 和时间；配对事件
包含客户端名称/IP；会话事件包含应用、客户端、会话 ID，以及可用的分辨率、
帧率或结束原因。

请求同时提供稳定的 delivery ID 和事件标识请求头，接收端可以用 delivery ID
对自动重试造成的重复投递去重。

内容规则：

- UTF-8 JSON。
- Markdown 内容最大 4096 字节。
- 超长内容在 UTF-8 字符边界处截断并追加省略号。
- 时间使用 Sunshine 主机的系统时区，格式固定为 `YYYY-MM-DD HH:mm:ss.xxx`。
- 日志不记录 URL、payload、凭据或响应正文。

测试接口发送相同的 payload 外层结构，固定使用 `event_id=-1` 和
`event_type=webhook_test`。`webhook_retries` 表示失败后的额外重试次数，
默认 0，允许 0–3；该参数只用于当前测试，不会写入配置文件。
测试通知与正式通知使用相同的运行时语言规则：`locale=zh` 或 `zh_TW`
时使用中文，其他语言使用英文。修改主配置中的 `locale` 后，需要重启
Sunshine 才会影响 Webhook 内容。

英文 Markdown 测试 payload：

```json
{
  "event_id": -1,
  "event_type": "webhook_test",
  "markdown": {
    "content": "**Sunshine Webhook Test**\n\n<font color=\"info\">**Test Notification**</font>\n\n>Result: <font color=\"comment\">Webhook endpoint reached</font>\n>Hostname: <font color=\"comment\">sunshine-host</font>\n>Event Type: <font color=\"comment\">webhook_test</font>\n>Sample Application: <font color=\"comment\">Sunshine Test Application</font>\n>Sample Client: <font color=\"comment\">Sunshine Test Client</font>\n>Sample Stream: <font color=\"comment\">1920x1080, 60 FPS, Audio Enabled</font>\n>Time: <font color=\"comment\">2026-07-27 09:20:29.979</font>\n"
  },
  "msgtype": "markdown"
}
```

中文 Markdown 测试 payload 的协议字段保持不变，仅翻译展示内容：

```json
{
  "event_id": -1,
  "event_type": "webhook_test",
  "markdown": {
    "content": "**Sunshine Webhook 测试**\n\n<font color=\"info\">**测试通知**</font>\n\n>结果: <font color=\"comment\">Webhook 接收地址已收到测试请求</font>\n>主机名: <font color=\"comment\">sunshine-host</font>\n>事件类型: <font color=\"comment\">webhook_test</font>\n>示例应用: <font color=\"comment\">Sunshine 测试应用</font>\n>示例客户端: <font color=\"comment\">Sunshine 测试客户端</font>\n>示例串流: <font color=\"comment\">1920x1080，60 FPS，音频已启用</font>\n>时间: <font color=\"comment\">2026-07-27 09:20:29.979</font>\n"
  },
  "msgtype": "markdown"
}
```

`Hostname` 和 `Time` 由 Sunshine 在发起测试时生成；应用、客户端和串流字段是明确标注的样例，
用于让接收端一次看到接近生产通知的 Markdown 结构，不代表当前存在真实串流会话。

## 投递语义

- 任意 `2xx` 视为成功。
- 传输错误和 HTTP `408`、`429`、`500`、`502`、`503`、`504` 可以重试。
- 默认最多三次尝试；POST 为“至少一次”投递，接收端应按 delivery ID 去重。
- HTTP 客户端底层的隐式重连已关闭；所有重试都由 Webhook dispatcher
  计数和调度，不会在 `attempts` 之外偷偷重放 POST。
- 请求显式发送 `Connection: close`。Webhook 工作线程常驻，但每次 HTTP/TCP
  连接只服务一次投递尝试，不进行连接池复用。
- 最多跳过 8 个连续临时 `1xx` 响应，超出后按协议错误结束本次尝试。
- 完整响应头解析后立即按状态码完成请求并关闭客户端，不读取响应正文；
  流式或迟迟不结束的正文不会占用在途槽或触发额外重试。
- 支持 `429/503` 的 `Retry-After`，等待上限为 60 秒。
- 最多两个异步请求同时在途；一条慢请求不会同步阻塞另一条请求。
- 生产事件每分钟最多接收 20 条；队列和速率均有限制，Webhook 过载不会反向阻塞 Sunshine 主流程。
- Webhook 使用独立 I/O 线程。启动失败、证书加载失败、投递异常和队列过载
  均只使 Webhook 降级；配置仍可保存，Sunshine 主流程继续运行。
- 初次启动失败后，用户保存或测试时会幂等重试启动；Sunshine 开始退出后
  永久禁止再次启动，并由生命周期 guard 取消请求、timer 和线程。

完整架构、接口、投递语义和安全边界见
[`webhook_design.md`](webhook_design.md)。
