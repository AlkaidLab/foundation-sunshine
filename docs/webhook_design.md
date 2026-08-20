# Sunshine Webhook 实现设计

## 1. 功能定位

Webhook 是 Sunshine 的可选通知模块，用于把配对、应用和会话事件异步发送到
用户配置的 HTTP 或 HTTPS 接收地址。

实现遵循以下边界：

- Webhook 故障不能终止或阻塞 Sunshine 主流程。
- Webhook 配置独立于 `sunshine.conf` 保存并支持热生效。
- 用户自行决定是否跳过 HTTPS 证书校验，默认严格校验。
- 事件投递采用有界队列、有限并发和有限重试。
- 接收端应按 delivery ID 处理可能出现的重复投递。
- 日志和 API 错误不得泄露 Webhook URL、查询参数、payload、凭据或本地路径。

## 2. 模块结构

| 模块 | 职责 |
|---|---|
| `src/webhook/webhook.cpp` | 活动配置快照、事件过滤、异步队列、并发投递、重试和生命周期 |
| `src/webhook/webhook_auth.cpp` | 独立配置路径、严格 JSON 解析、备份和原子写入 |
| `src/webhook/webhook_api.cpp` | 已认证的配置读取、配置保存和测试投递业务处理 |
| `src/webhook/webhook_format.cpp` | 事件 payload、时间戳、UTF-8 截断和测试 payload |
| `src/webhook/webhook_client_base.h` | HTTP/HTTPS 共用的响应头专用异步读取 |
| `src/webhook/webhook_httpclient.*` | 禁止隐式重连的 HTTP Client |
| `src/webhook/webhook_httpsclient.*` | TLS 配置、证书校验和禁止隐式重连的 HTTPS Client |
| `src/confighttp.cpp` | 在现有 Web UI HTTPS 服务中注册并认证 Webhook 路由 |
| `WebhookCard.vue` | 网络页中的 Webhook 卡片、懒加载弹窗、测试和独立保存 |
| `webhookService.js` | Web UI 的同源 Webhook API 调用 |
| `webhookConfig.js` | 前后端 DTO、秒/毫秒转换和事件编号规范化 |

Webhook 沿用项目现有的 C++、Boost.Asio、Simple-Web-Server、libcurl URL API、
nlohmann/json、Vue 3、vue-i18n 和请求工具，没有引入新的网络框架。

## 3. 配置模型

### 3.1 配置位置

有效配置文件位于实际选用的 `sunshine.conf` 同目录：

```text
config/
├── sunshine.conf
└── webhook_auth.json
```

配置文件名固定为 `webhook_auth.json`，不是
`credentials/webhook_auth.json`。

`sunshine.conf` 中可能残留的旧 Webhook 字段不迁移、不读取、不回退使用。
这些字段只按普通未知配置参与 `/api/config` 的通用读写，不影响当前 Webhook。

### 3.2 文件格式

配置必须是只包含以下五个字段的 JSON 对象：

```json
{
  "webhook_enabled": false,
  "webhook_events": "0,1,2,3,4,5,6",
  "webhook_skip_ssl_verify": false,
  "webhook_timeout": 5000,
  "webhook_url": ""
}
```

字段规则：

| 字段 | 类型 | 规则 |
|---|---|---|
| `webhook_enabled` | boolean | 是否发送生产事件 |
| `webhook_url` | string | HTTP/HTTPS URL，最大 4096 字节 |
| `webhook_skip_ssl_verify` | boolean | 仅影响 HTTPS；默认 `false` |
| `webhook_timeout` | integer | 毫秒，范围 1000–15000，默认 5000 |
| `webhook_events` | string | `0..6` 的逗号列表；`-1` 表示空选 |

文件缺失时使用“关闭、空 URL、严格证书校验、5000ms、事件全选”的默认配置，
且不输出配置错误。文件存在但为空、过大、JSON 损坏、字段缺失、字段多余或类型
不正确时判定为损坏；Webhook 被禁用，Sunshine 继续运行。

### 3.3 保存与热生效

配置保存按以下顺序串行执行：

1. 校验并规范化全部字段。
2. 完整分配一份尚未对事件入口可见的不可变配置快照。
3. 把新 JSON 写入同目录临时文件。
4. 若旧配置存在，将旧文件原始字节备份为 `webhook_auth.json.bak`。
5. 原子替换主配置文件。
6. 磁盘保存成功后，原子发布新的活动配置快照。

备份或主文件写入失败时，活动配置保持旧值。备份文件只用于人工恢复，启动时
不会自动读取。已经排队或正在重试的投递保留其接收事件时的配置快照；保存后
产生的新事件立即使用新配置。

`webhook_auth.json`、临时文件和备份文件均被 Git 忽略。POSIX 平台会在写入
敏感内容之前把临时文件权限限制为文件所有者读写。

## 4. Web API

Webhook API 注册在现有 Web UI HTTPS 服务中，复用相同认证逻辑。JSON 响应
统一设置：

- `Cache-Control: no-store`
- `Content-Type: application/json`
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY`

请求正文上限为 64 KiB。错误响应使用稳定的通用信息，不回显 URL、文件路径或
文件内容。

### 4.1 读取配置

```http
GET /api/webhook/config
```

打开 Webhook 卡片弹窗时异步调用。配置页面初始加载和 `/api/config` 不读取
Webhook 独立文件。

成功响应：

```json
{
  "status": true,
  "webhook_enabled": false,
  "webhook_events": "0,1,2,3,4,5,6",
  "webhook_skip_ssl_verify": false,
  "webhook_timeout": 5000,
  "webhook_url": ""
}
```

配置文件损坏或无法读取时返回通用 500，并使用稳定错误码
`webhook_config_invalid`，供前端显示简短的清理配置文件提示。

### 4.2 保存配置

```http
POST /api/webhook/config
Content-Type: application/json
```

请求必须精确包含五个配置字段。保存成功后立即热应用，不依赖网络页外层的
“保存”或“应用并生效”按钮。

成功响应：

```json
{
  "status": true,
  "runtime_active": true
}
```

`status=true` 表示配置已经持久化并发布。`runtime_active=false` 表示配置已保存，
但 Webhook 运行时当前无法启动；前端会提示通知服务不可用，Sunshine 仍继续运行。

保存只校验结构、范围和 URL 语义，不要求远端接收地址当前可达。用户可以通过
独立测试接口检查网络连通性。

### 4.3 测试投递

```http
POST /api/webhook/test
Content-Type: application/json
```

请求：

```json
{
  "webhook_retries": 0,
  "webhook_skip_ssl_verify": false,
  "webhook_timeout": 5000,
  "webhook_url": "https://example.invalid/webhook"
}
```

- 不要求先启用 Webhook。
- 不读取或修改持久化配置。
- `webhook_retries` 可省略，默认 0，范围 0–3，表示失败后的额外重试次数。
- 测试使用生产 transport 和相同 payload 外层结构。
- 测试事件固定为 `event_id=-1`、`event_type=webhook_test`。
- 测试内容与正式通知使用相同的语言规则：`zh`、`zh_TW` 使用中文，
  其他 Sunshine locale 使用英文。
- 配置服务线程只保留异步响应，不同步等待远端网络。

响应：

```json
{
  "attempts": 1,
  "error": "none",
  "http_status": 204,
  "status": true
}
```

`error` 使用稳定分类：`none`、`not_running`、`queue_full`、`rate_limited`、
`invalid_url`、`transport`、`http_status`、`cancelled` 或 `internal`。

## 5. Web UI

Webhook 保留在网络配置页原有位置，以大卡片和弹窗呈现，不创建独立通知页签。

界面行为：

- 初始只显示卡片，点击后才异步读取独立配置。
- URL 支持 HTTP 和 HTTPS，内容默认隐藏并可通过显隐按钮查看；HTTP 显示明文传输提醒。
- Webhook 通知和“跳过 HTTPS 证书校验”使用启停开关；证书校验被跳过时显示明确风险提示。
- 超时在界面中使用 1–15 秒整数，API 和文件中继续使用毫秒。
- 七个事件使用多选框，默认全选，可全选或清空。
- 保存、测试和加载状态互斥，避免并发编辑造成状态错报。
- 保存成功后关闭弹窗，并通过页面顶部居中的通知显示热生效结果，不依赖外层配置按钮。
- 配置损坏时提示用户清理 `webhook_auth.json`，不显示本地绝对路径。
- 同时提供 Linux/macOS shell 与 Windows PowerShell 的 curl 请求模板。
- 弹窗只允许通过“取消”或保存成功关闭；点击遮罩或按 Escape 不关闭，并保留键盘焦点循环和关闭后焦点恢复。
- 弹窗正文支持鼠标滚轮和触摸纵向滑动，并隔离滚动越界，避免带动背景页面。
- 浅色模式使用不透明弹窗表面和高对比度正文、标签及辅助文字。

所有 Webhook 文案和七个事件名称使用项目现有 vue-i18n 体系。

## 6. 事件与 Payload

### 6.1 事件编号

| 编号 | `event_type` | 事件 |
|---:|---|---|
| 0 | `config_pair_success` | 配对成功 |
| 1 | `config_pair_failed` | 配对失败 |
| 2 | `nv_app_launch` | 应用启动 |
| 3 | `nv_app_resume` | 应用恢复 |
| 4 | `nv_app_terminate` | 应用终止 |
| 5 | `nv_session_start` | 会话开始 |
| 6 | `nv_session_end` | 会话结束 |

事件编号属于持久化格式和接收端协议，不能因代码中的枚举顺序变化而修改。

### 6.2 Payload

生产事件使用 UTF-8 JSON。以下是当前默认 Markdown 格式下的一次完整应用启动
上报样例。所有地址、名称、ID 和时间均为虚构值；请求头顺序不属于协议：

```http
POST /webhook HTTP/1.1
Host: example.invalid
Content-Type: application/json; charset=utf-8
Connection: close
User-Agent: Sunshine_Foundation/1.0 (System Notification Service)
X-Webhook-Delivery: 018f0000-0000-7000-8000-000000000001
X-Trace-ID: 018f0000-0000-7000-8000-000000000001
X-Webhook-Event-ID: 2
X-Webhook-Event: nv_app_launch
X-Timestamp: 1785115229979
X-Hostname: sunshine-host
X-Signature: 1234567890
X-Auth-Token: Sunshine_Foundation_9979
X-Client-ID: Sunshine_Foundation
X-API-Version: v1.0
X-Client-Info: Foundation Sunshine
X-Service-Name: Sunshine_Foundation_Service
X-Component: Sunshine_Foundation_Component

{"event_id":2,"event_type":"nv_app_launch","markdown":{"content":"**Sunshine System Notification**\n\n<font color=\"info\">**Application Launched**</font>\n\n>Hostname: <font color=\"comment\">sunshine-host</font>\n>Server IP: <font color=\"comment\">192.168.1.10</font>\n>App Name: <font color=\"comment\">Example App</font>\n>App ID: <font color=\"comment\">123</font>\n>Client: <font color=\"comment\">Moonlight Client</font>\n>Client IP: <font color=\"comment\">192.168.1.20</font>\n>Resolution: <font color=\"comment\">1920x1080</font>\n>FPS: <font color=\"comment\">60</font>\n>Audio: <font color=\"comment\">Enabled</font>\n>Time: <font color=\"comment\">2026-07-27 09:20:29.979</font>\n"},"msgtype":"markdown"}
```

示例中的 `X-Signature` 只展示数值形态；实际值由当前 C++ `std::hash`
实现生成，不具备跨平台稳定性，也不构成密码学签名。

为便于阅读，body 也可以展开为：

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

不同事件只输出实际存在的内容字段：

| 事件 | `markdown.content` 中的事件字段 |
|---|---|
| 配对成功/失败 | Client Name、Client IP；失败异常时可包含 Error |
| 应用启动/恢复 | App Name、App ID、Client、Client IP、Resolution、FPS、Audio |
| 应用终止 | App Name、App ID |
| 会话开始 | App Name、Client、Client IP、Session ID、Resolution、FPS |
| 会话结束 | App Name、Client、Client IP、Session ID、End Reason |

所有生产事件还会包含 Hostname、可用时的 Server IP 和 Time。空值不输出。

Markdown 内容最大 4096 字节。超长内容在 UTF-8 字符边界处截断并追加省略号。
时间使用 Sunshine 主机的系统时区，格式固定为 `YYYY-MM-DD HH:mm:ss.xxx`。

测试按钮使用相同外层结构。语言读取 Sunshine 本次启动时加载的
`config::sunshine.locale`；修改主配置语言后需要重启 Sunshine 才会影响
Webhook。英文 Markdown payload 为：

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

中文 payload 保持 `event_id`、`event_type`、`msgtype` 等协议字段不变，
仅翻译接收端展示的文本：

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

真实测试中的主机名和时间由 Sunshine 动态填写；其余带 `Sample` 的字段只展示接收端可能看到的
生产内容形态，不对应当前真实应用或客户端。

### 6.3 请求头

每次投递包含：

- `Content-Type: application/json; charset=utf-8`
- `Connection: close`
- `User-Agent`
- `X-Webhook-Delivery`：稳定 delivery ID
- `X-Trace-ID`：与 delivery ID 相同
- `X-Webhook-Event-ID`
- `X-Webhook-Event`
- `X-Timestamp`
- `X-Hostname`

既有 `X-Signature`、`X-Auth-Token`、`X-Client-ID`、`X-API-Version`、
`X-Client-Info`、`X-Service-Name` 和 `X-Component` 仅作为兼容元数据保留。
当前配置没有用户密钥，因此这些字段不构成密码学身份认证；接收端不能把它们
当作访问控制依据。

动态主机名字段会替换 ASCII 控制字符并限制为 512 字节，避免请求头注入。

## 7. URL 与 TLS

URL 规则：

- 仅支持 `http` 和 `https`。
- 必须具有非空 authority 主机。
- 不接受内嵌用户名或密码。
- 不接受 ASCII 空白、控制字符或超过 4096 字节的值。
- 保留 path 和 query，fragment 不发送到接收端。
- 不自动跟随重定向。
- 不使用 Webhook 专用代理配置。

HTTPS 默认同时校验证书链和目标主机身份。只有用户明确设置
`webhook_skip_ssl_verify=true` 时才使用 `verify_none`。
域名目标会发送 SNI 并检查 OpenSSL 设置结果；IPv4/IPv6 字面量按照 RFC 6066
不发送 SNI，开启证书校验时仍按 IP 身份校验证书。

Windows 打包环境中的 OpenSSL 默认 CA 路径不保证可用，因此开启证书校验时：

1. 通过 Windows CryptoAPI 以只读方式打开 `LocalMachine\ROOT` 和
   `CurrentUser\ROOT`。
2. 只枚举公开 X.509 证书，不读取私钥、不修改系统证书库。
3. 把可用信任根导入 Webhook 独立的 OpenSSL context。
4. 继续由 OpenSSL 完成证书链和主机身份校验。

系统 ROOT 无法打开或没有可用信任根时，本次 HTTPS 投递按传输失败处理，不会
自动跳过证书校验，也不会影响 Sunshine。

当前实现没有接入 Windows 原生证书链策略，因此不会完整继承 Windows 链引擎的
全部吊销、Disallowed 和动态链构建行为。非 Windows 平台使用 OpenSSL 默认信任
路径。

## 8. 异步投递模型

### 8.1 线程与并发

Webhook 维护一个专属 `io_context` 工作线程。线程常驻以支持事件接收、定时器和
配置热启用，但 DNS、TCP、TLS、写入和响应读取均为异步操作。

运行限制：

- 最多 64 个未完成投递。
- 最多 2 个网络请求同时在途。
- 生产事件每分钟最多接收 20 条。
- 测试请求不消耗生产事件速率额度。
- 队列或速率超限时丢弃新事件并限频记录警告，不阻塞事件产生方。

单个慢请求等待网络或超时时，同一 I/O 线程仍可推进另一条请求、重试 timer 和
关闭操作，不会形成同步串行阻塞。

### 8.2 超时

配置继续使用毫秒，但 Simple-Web-Server 的底层秒级超时使用向上取整：

```text
timeout_seconds = max(1, ceil(webhook_timeout_ms / 1000))
```

每次尝试另有同一 `io_context` 上的总截止 timer，从 DNS 开始覆盖 TCP、TLS、
写入和等待响应头的完整过程。到期后停止当前 client，并按传输失败策略处理。

### 8.3 短连接和响应处理

每次尝试创建独立 HTTP/HTTPS Client，请求显式发送 `Connection: close`。
Webhook 工作线程常驻，但网络连接不常驻，也不使用连接池。

Webhook 的接收确认契约只依赖 HTTP 状态码和响应头：

- 使用最大 16 KiB 的独立响应头缓冲区。
- 正确跳过除 `101` 外的临时 `1xx` 响应。
- 连续临时 `1xx` 最多接受 8 个，超出后按协议错误结束本次尝试。
- 取得最终响应头后立即完成尝试并关闭 client。
- 不读取或解释响应正文。

因此慢响应体或流式响应不会继续占用两个在途槽，也不会把已返回成功状态的 POST
误判为超时重试。接收端不应把业务成败只放在 `2xx` 响应正文中。

## 9. 状态码与重试

结果策略：

- 任意 `2xx` 为成功。
- 传输错误可以重试。
- HTTP `408`、`429`、`500`、`502`、`503`、`504` 可以重试。
- 其他 HTTP 状态不重试。
- 生产事件最多尝试 3 次。
- 测试请求按 `webhook_retries` 进行 0–3 次额外重试，总尝试次数为 1–4。
- `429` 和 `503` 支持 `Retry-After` 的秒数或 HTTP-date，上限 60 秒。
- 无 `Retry-After` 时使用约 1 秒、2 秒并带小幅抖动的退避。

Simple-Web-Server 的底层隐式重连在 Webhook 专用 Client 中关闭，所有尝试都由
dispatcher 明确计数。

POST 使用“至少一次”投递语义。如果远端已经处理请求、但响应头未被成功接收，
Sunshine 可能重试。相同逻辑投递的 delivery ID 在重试之间保持不变，接收端应
据此去重。

## 10. 生命周期与故障隔离

Webhook 运行时在 Sunshine 启动阶段建立生命周期守卫。即使当前未启用 Webhook，
工作线程也可以安静启动并保持空闲，使后续保存配置能够立即热启用。

故障隔离规则：

- Webhook 初次启动失败时只记录通用错误，Sunshine 继续启动。
- 保存或测试可以幂等重试启动运行时。
- 运行时不可用时，生产事件被安全丢弃，不反向阻塞主流程。
- API、事件入口、I/O handler、完成回调和关闭路径均有异常边界。
- Sunshine 开始退出后永久拒绝重新启动 Webhook。
- 关闭时停止接收新投递，取消队列、重试 timer、总截止 timer 和活动 client，
  完成等待中的测试回调，停止 `io_context` 并 join 工作线程。
- dispatcher 析构提供最终幂等回收兜底。

Webhook 配置和网络故障不会传播为主程序退出条件。

## 11. 日志与隐私

允许记录：

- delivery ID
- 事件类型
- 尝试次数
- HTTP 状态码
- 总耗时
- 稳定错误类别

禁止记录或回显：

- Webhook URL、host、path 或 query
- payload 和响应正文
- 凭据或 token
- 本地配置绝对路径
- 证书内容

配置文件损坏、证书库不可用和网络错误只返回不包含本地细节的稳定错误。

## 12. 协议依据

- [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986.html)：URI scheme、authority、path、query 和 fragment。
- [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html)：HTTP 状态码、`Retry-After` 和连接语义。
- [RFC 6585](https://www.rfc-editor.org/rfc/rfc6585.html)：`429 Too Many Requests`。
- [RFC 9525](https://www.rfc-editor.org/rfc/rfc9525.html)：HTTPS 服务身份校验。
- [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259.html)：UTF-8 JSON。
- 时间字段：使用 Sunshine 主机系统时区和固定格式 `YYYY-MM-DD HH:mm:ss.xxx`。
