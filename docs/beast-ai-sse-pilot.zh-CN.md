# Beast AI/SSE 本地试点

试点用于评估 Web UI / GameStream 的 HTTP 会话层迁移。默认不构建，独立绑定环回地址，不注册 Sunshine 的生产路由，也不启动采集、编码或驱动服务。

## 生产迁移进度

独立试点验证通过后，生产迁移按层推进。每一步先补对应回归、检查结果，再扩大替换范围；失败的用例修复并重跑前，不推进下一层。

| 阶段 | 范围 | 验证门槛 | 进度 |
| --- | --- | --- | --- |
| 1 | Web UI / GameStream 共用的 AI/SSE 响应编码 | 错误状态/正文、成功终止块、截断检测、跨块数据、认证、断连与关闭 | 本地验证通过 |
| 2 | AI 请求解析和 TLS 会话生命周期 | 原端口/URL、Web UI 登录与 Origin、GameStream 配对身份、首包前取消、双监听 | 已接入；新增保存配对证书和双地址监听验收 |
| 3 | 其他只读 HTTP 路由 | 路由清单、响应兼容性、长连接、并发与客户端对照 | 已接入；路由清单与逐条 TLS 分发验收 |
| 4 | 配置修改、上传和其余路由 | 权限、请求上限、落盘行为、关闭与失败回归 | 已接入；权限、JSON、分块上传与落盘验收 |

阶段 1 使用 `src/ai/beast_response.h` 的 Beast serializer/chunk helpers，将结果写入现有响应缓冲区，网络发送仍异步进行。`ai::http::stream_response` 不再手写 SSE 响应头；成功上游输出 chunked 结束块，失败或取消时保持报文截断，空正文成功响应包含 Content-Length: 0，错误响应保留原状态映射与正文。

阶段 1 已进入生产共用函数，不受独立程序的 `SUNSHINE_AI_BEAST_PILOT` 开关控制。阶段 2–4 接入后，Web UI 与 GameStream 的监听和会话由 `src/http/server.*` 共用 Beast 实现；Web UI 认证、Origin 策略及路由注册仍由业务模块负责，GameStream 在 TLS 握手后继续调用原配对验证函数，将验证得到的 UUID 保存在连接中并传给每个请求。

生产 AI 流已启用首包前断连观察，新增真实 curl/TLS 回归验证取消传播。HTTP/1.1 keep-alive、流水线请求、分块上传、100 Continue、歧义 Content-Length、请求头和正文上限、CA mTLS、配对身份跨请求保持、关闭后的响应释放均有真实网络测试。非标准 444 状态保留，但改为完整 Beast HTTP 报文；不再发送只有状态行的截断响应。

服务端迁移候选在 Windows 完整构建与 CTest 28/28 通过，包括通用传输测试 14/14、生产 HTTP 安全测试 17/17，以及 Web UI 路由与认证 12/12。

Webhook 与测试客户端已改用共用的 `src/http/client.*`：Beast 负责请求序列化、最终响应头与正文解析；请求具有整体和建连截止时间，不隐式重放 POST。Webhook 只读取最终响应头，保留原状态重试策略，停机时等待取消回调释放资源。系统信任加载复用 Windows 机器/用户 ROOT 存储；Windows 传输库与主程序统一使用静态 OpenSSL，避免 DLL 与静态库混用 opaque 对象。

客户端迁移后完整 Windows 构建与 CTest 29/29 通过。通用网络测试 19/19，生产 HTTP 安全测试 17/17，另有独立 Webhook 生命周期测试，覆盖仅响应头完成、429/503 显式重试与相同投递 ID、400 不重试、断连不隐藏重放、超大响应头拒绝、默认 TLS 验证、显式跳过验证和停机取消全部排队/活动任务。localhost TLS 对照使用产品默认 5 秒预算，另覆盖 2 秒预算的 IPv4 TLS 直连；超时和正文限制独立校验。

源代码、测试和构建图已移除 Simple-Web-Server，旧子模块不再进入版本控制或构建。试点现在通过 `web::Server<web::HTTPS>` 和 `ai::http::StreamExecutor` 复用生产 TLS/HTTP/SSE 会话，仅保留环回绑定、授权与请求校验策略。生产默认四个流任务，试点使用独立、可配置容量的同一执行器；不再维护第二套 TLS 读写与 chunk 发送实现。HTTP/1.0 SSE 在调用上游前返回 505。最终提交的完整构建、开关 ON/OFF 回归、远端 CI 和 CodeRabbit review 结果记录在对应 PR 中。

阶段 1 分组结果：上游/错误回归 8/8、编码回归 3/3、断连/关闭回归 2/2；HTTP 安全目标 16/16、含试点的对照目标 28/28、Web UI 路由与认证目标 12/12。Windows 完整构建通过；启用独立试点时 CTest 28/28，关闭时 CTest 27/27。编码回归检查实际 TLS 报文，覆盖嵌入 NUL、1 字节到跨多个 curl 回调的正文，以及上游 Content-Length 不足时的客户端截断错误。

新增验收使用保存的配对状态与原 `verify_client_certificate` 策略进行真实 TLS 握手，覆盖可信/不可信设备、长连接身份及伪造 UUID 请求头；另验证两个地址同端口监听互不影响。路由矩阵逐条检查方法、路径、查询和捕获组的 TLS 分发，替换测试处理器避免执行重启或设备操作；Basic 登录通过模拟远端地址验证，封面上传用真实处理器检查分块请求与二进制落盘。测试不代表其他平台、浏览器 UI 操作或物理 Moonlight 客户端已验收。

## 公共会话协程与当前本地验收

公共会话的 TLS 握手、HTTP 请求头、100 Continue 与正文读取已改用 Asio `awaitable` / `co_spawn` / `co_await`。会话以 `shared_ptr` 参数进入协程帧，临时响应在等待发送完成期间保存在帧中；网络错误仍走原错误处理，意外异常由完成处理器关闭连接。发送队列、SSE 断连探测、授权策略和阻塞 curl 任务池保持原有职责。

协程阶段的 Windows Release 完整构建通过：试点 ON 时 CTest 30/30，OFF 时 29/29，OFF 构建图不生成试点目标。通用传输 22/22、生产 HTTP/SSE 安全 18/18、含试点对照 31/31；新增用例覆盖挂起的 TLS 握手、请求头与正文读取在停机时取消，并检查请求对象释放。Web UI 17 项回归覆盖路由清单、逐条 TLS 分发、Origin、模拟远端 Basic 和真实分块封面落盘；原配对策略的真实 TLS 验收也通过。审查回归还覆盖超过 16 KiB 的 backend 数据块、IPv4/IPv6 同端口、接收端启动异常线程回收，以及解除配对后旧长连接的下一次请求拒绝。公共配对策略在请求分发前重新校验；accept 资源错误有可取消的退避。

日志、静态文件和 GameStream 应用封面使用同一 `write_file` / Beast `file_body` 路径，以打开句柄的初始大小生成响应并有界发送。Windows 共享读取允许日志继续写入或轮转；回归在 64 KiB 响应队列上限下下载 65 MiB 中文路径日志，并保持写句柄打开。应用封面的真实 TLS 回归先复现旧缓冲路径返回 400，再验证 65 MiB 图片完整下载、缺失文件返回 404、缺失参数返回 400。普通内存响应的上限仍保留。非法 Base64 封面上传的真实 TLS 回归先复现响应截断，再验证有效的 400 JSON 响应且不创建文件。可选的 socket 发送缓冲设置使用非抛异常的错误码接口，避免单连接错误终止监听器；Webhook 停机测试的完成计数由回调共享持有，提前退出也不会留下悬空引用。远端 CI 与 CodeRabbit 的最终结果以 PR 为准。

## 接入边界

- `src/ai/beast_server.*`：环回试点策略适配；共享 `src/http/server.*` 的请求解析/TLS/关闭流程和 `src/ai/http_stream.*` 的 SSE 编码、容量、超时与取消。
- 授权策略与 AI 后端通过回调注入。策略接收请求、远端地址和客户端证书 SHA-256；两个回调均必须提供。
- 上游继续使用生产 `ai::processAiChatStream` / curl，保留 AI 配置适配、401/429 正文和其他上游错误的 502 映射。
- 独立程序从指定的 `ai_config.json` 读取配置，同目录的 `ai_llm_credential.bin` 通过现有凭据存储读取；不修改生产配置或凭据。
- 为独立程序复用 AI 代理，将通用 `http::post_json` 函数移到 `src/http_json.cpp`。其请求逻辑保持原样；私有写回调保留 10 MiB 上限，并捕获异常。

## 构建与运行

在正常 Sunshine 构建配置的基础上开启试点：

```powershell
cmake -S . -B build -DSUNSHINE_AI_BEAST_PILOT=ON -DBUILD_TESTS=ON
cmake --build build --target ai_beast_pilot ai_beast_pilot_tests --parallel 4
ctest --test-dir build -R '^ai_beast_pilot_tests$' --output-on-failure
```

独立程序不安装进 Sunshine 发布包。使用已准备好的 TLS 证书、私钥和令牌文件：

```powershell
.\build\ai_beast_pilot.exe cert.pem key.pem pilot-token.txt ai_config.json 0
```

默认监听 `127.0.0.1`，端口 `0` 表示由系统分配，启动时输出实际 URL。令牌文件包含一行 16–4096 字节的令牌，程序不输出令牌。按 Ctrl+C 停止。

独立程序唯一业务端点是 HTTP/1.1 的 `POST /api/ai/chat/completions`，要求：

- `Authorization: Bearer <令牌文件内容>`，重复授权头被拒绝。
- `Content-Type: application/json`；可附带 charset 参数。
- OpenAI 兼容请求正文，包含 `"stream": true`。

该端点的 OPTIONS 返回预检信息。它不开放任意 Origin，也不复用生产 Web UI 的登录凭据。嵌入式服务器的授权策略可只使用 mTLS 证书指纹；独立程序的策略始终要求 Bearer 令牌。

可选参数：

```text
ai_beast_pilot <cert.pem> <key.pem> <token-file> <ai_config.json> [port] [client-ca.pem] [duration-seconds]
```

`client-ca.pem` 非空时强制验证客户端证书；设置运行秒数可执行自动冒烟测试，默认 `0` 表示持续运行。没有客户端 CA 时，可传空字符串来设置运行秒数。

## 资源与生命周期

默认 2 个 I/O 线程、最多 4 个上游任务、1 MiB 请求正文和 8 KiB 请求头。上游槽位耗尽时立即返回 503 与 Retry-After，不排队。

每个 SSE 会话只保留一个最多 16 KiB 的待发送块；更大的 backend 数据按此大小拆分。后台 curl 回调等待实际写入完成，默认发送超时 5 秒；HTTP I/O 线程保持异步。成功流用 Beast 序列化响应头、chunk framing 和终止块；错误正文经过上游状态判断后发送普通 HTTP 响应。

请求读取完成后保留一个 TLS 读取操作，与写入并行，监听客户端 FIN/RST。这样首个上游数据到达前的断连也能触发 curl 的取消轮询。已有流仍发送 SSE 注释心跳。

关闭顺序为：停止流任务接收并请求上游取消，在 I/O 仍可运行时等待任务退出，然后停止监听、取消连接、排空回调并等待 I/O 线程退出。会话和发送缓冲区由回调持有，不依赖监听器栈对象的生命周期。

## 独立试点初次验证结果

Windows Release，GCC 15.2 / Boost 1.92，复用本地依赖缓存：

| 验证 | 结果 |
| --- | --- |
| 现有正常 SSE 与 Beast 的状态/正文对照 | 一致；Beast 正确生成 chunked 终止块 |
| 401、429、503，含空正文与 100 Continue | 与现有实现一致，无提前 200/SSE |
| 授权、重复授权头、JSON/方法/请求大小检查 | 上游调用前拒绝 |
| 首个上游数据前客户端断开 | 上游在 3 秒测试上限内取消 |
| 单个 I/O 线程、两个满载上游任务 | OPTIONS 可响应；超限返回 503；断连后槽位释放 |
| 不读取响应的慢客户端 | 1 秒发送超时配置下中止；没有无限发送队列 |
| 上游静默时停止服务 | 3 秒测试上限内退出，后台任务已回收 |
| TLS 与 mTLS | 服务端证书/主机名校验通过；无证书及不可信客户端被拒绝；可信客户端指纹传入策略 |
| 独立程序冒烟 | 无授权 401，授权后真实 curl SSE 转发成功，定时关闭退出码 0 |

试点测试包含 12 项 Beast 回归与 13 项现有安全回归，25/25 通过，另覆盖 HTTP/1.0 拒绝和歧义 Content-Length。开启试点时完整构建与 28/28 CTest 通过；关闭试点时目标不生成，正常 Sunshine 完整构建与 27/27 CTest 通过。

## 验收范围与后续运行验证

试点证明了 Beast 的协议序列化与可控会话生命周期能改善首包前取消和关闭流程。代价是需要维护监听、会话、路由适配和授权接口；Beast 本身不提供完整业务框架。

Web UI 认证/Origin、GameStream 配对证书到设备身份、双地址监听和路由分发已接入公共传输层。CA mTLS 和 Moonlight 配对校验是独立策略，后者仍使用原配对状态。此次代码迁移保留业务路由和阻塞 curl 上游，以有上限的任务池隔离 AI 流。真实网络 DNS/TLS 时延、Linux/macOS 构建、浏览器与物理 Moonlight 操作、吞吐量和长时间负载尚未验收；本地时限断言均使用环回上游。
