# Beast AI/SSE 本地试点

试点用于评估 Web UI / GameStream 的 HTTP 会话层迁移。默认不构建，独立绑定环回地址，不注册 Sunshine 的生产路由，也不启动采集、编码或驱动服务。

## 接入边界

- `src/ai/beast_server.*`：Beast 请求解析、TLS、分块 SSE 响应、发送超时、连接关闭和取消传播。
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

每个会话只保留一个最多 16 KiB 的待发送块。后台 curl 回调等待实际写入完成，默认发送超时 5 秒；HTTP I/O 线程保持异步。成功流用 Beast 序列化响应头、chunk framing 和终止块；错误正文经过上游状态判断后发送普通 HTTP 响应。

请求读取完成后保留一个 TLS 读取操作，与写入并行，监听客户端 FIN/RST。这样首个上游数据到达前的断连也能触发 curl 的取消轮询。已有流仍发送 SSE 注释心跳。

关闭顺序为：停止接收、取消所有会话、等待上游任务退出、释放 I/O 工作守卫并排空回调、等待 I/O 线程退出。会话、缓冲区和序列化器由回调持有，不依赖监听器栈对象的生命周期。

## 已验证结果

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

## 后续迁移判断

试点证明了 Beast 的协议序列化与可控会话生命周期能改善首包前取消和关闭流程。代价是需要维护监听、会话、路由适配和授权接口；Beast 本身不提供完整业务框架。

进入生产迁移前仍需实现现有 Web UI 认证/Origin 策略适配、GameStream 配对证书到设备身份的绑定、环回与指定地址双监听，并用现有 93 条路由清单逐项验证。当前标准 CA 客户端证书校验不等价于 Moonlight 的配对证书策略。时限断言使用本地上游，真实网络的 DNS/TLS 建连、Linux/macOS 构建、吞吐量与长时间负载尚未验证。上游仍为阻塞 curl，资源通过独立任务上限隔离。
