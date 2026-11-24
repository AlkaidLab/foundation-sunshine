# HTTP / TLS / AI SSE 架构与回归

Web UI、GameStream HTTP/HTTPS 与 Webhook 共用 `src/http/` 的 Boost.Beast / Boost.Asio 传输。原 Simple-Web-Server 的源码依赖、子模块和构建引用已移除。AI/SSE 独立环回试点完成验证后退役；正式功能和测试无需试点程序或构建开关。

## 迁移范围

| 路径 | 当前实现与策略 |
| --- | --- |
| Web UI HTTPS 与附加环回监听 | `web::Server<web::HTTPS>`；`confighttp` 模块负责路由、Basic 登录、Origin 和业务权限 |
| GameStream HTTP / 配对 HTTPS | `web::Server<web::HTTP>` / `web::Server<web::PairedHTTPS>`；`nvhttp` 提供原配对证书验证和 UUID 策略，每个请求重新验证配对状态 |
| Web UI / GameStream AI SSE | 共用 `ai::http::stream_response`、`StreamExecutor` 和 Beast 响应编码 |
| Webhook HTTP / HTTPS | `web::Client`；仅读取最终响应头，业务调度器显式控制重试和投递 ID |
| 日志、静态资源、GameStream 封面 | 共用 `write_file` / Beast `file_body`，异步发送打开的文件句柄 |
| 文件映射 WSS | `src/file_mapping/file_mapping_ws_session.*` 的独立 Beast WebSocket 会话；管理和能力发现接口使用共用 HTTP 服务 |

本次迁移完整替换的是原 Simple-Web-Server 网络层，并非移除 libcurl。`src/ai/proxy.cpp` 的上游 SSE、`src/http_json.cpp` 的 JSON POST，以及 `src/httpcommon.cpp` 的 JSON GET、下载和受限封面下载继续使用 libcurl。上游 SSE 属于阻塞传输，运行在有界流执行器中；它不运行于 HTTP 会话的 I/O 线程。共用 Beast 客户端和 Webhook 还使用 libcurl URL API 解析地址，这些调用不执行网络传输。

## 模块职责

- `src/http/server.*`：HTTP/HTTPS 监听、解析、连接身份、keep-alive、响应发送队列、断连、截止时间及停机。TLS 握手、请求头、100 Continue 和正文读取使用 Asio 协程；会话由协程参数中的 `shared_ptr` 持有。每条连接的异步读写在 strand 上串行推进。
- `src/http/client.*`：DNS、连接、TLS 验证、Beast 请求和响应、整体/建连截止时间及取消。不隐式重放 POST，不决定业务重试策略。
- `src/http/types.*`、`headers.h`、`ostream.h`、`tls_trust.*`：共用应用类型、响应头校验、Beast 缓冲写适配及系统信任加载。Windows 共用静态 OpenSSL，并读取机器和用户 ROOT 存储。
- `src/ai/http_stream.*`：上游工作线程容量、SSE 背压、取消和关闭；生产默认最多四个流任务，无等待队列。`src/ai/beast_response.h` 负责 SSE 头、chunk、结束块和错误响应编码。
- `src/confighttp.cpp`：Web UI 监听及模块关闭；`src/confighttp/routes.cpp` 组合路由和特殊本地访问策略。`auth/common/types` 提供共用认证、响应和类型，各业务模块注册自己的处理器。
- `src/nvhttp/`、`src/webhook/`、`src/file_mapping/`：分别拥有 GameStream 业务、投递策略及文件映射协议/生命周期。共用传输层不依赖这些业务模块。

无需把 HTTP 与 WebSocket 的不同协议生命周期合成一个通用会话，也无需为已经共用的 AI 传输再保留一个服务端包装类。当前业务分组与传输职责分离；本次收尾不进一步拆分稳定的会话状态机。

## 流响应与文件边界

上游状态在首个数据回调前检查：401、429 等错误保留状态映射与正文，不提前发送 200 SSE。HTTP/1.0 SSE 在调用上游前返回 505。成功流使用 Beast chunk 编码并写结束块，取消或失败留下可检测的截断；空正文成功使用 Content-Length: 0。

后端数据按最多 16 KiB 切分，每次等待真实发送完成后才接纳下一块。慢客户端触发截止时间，不累计无限数据。首包前断连由连接探测取消上游，首包后的上游静默也会观察客户端断连。

停机顺序是先关闭流任务接纳、取消并排空上游任务，再停止 HTTPS 监听并回收线程，让任务仍能收到发送完成通知。普通文件下载使用 `file_body`，不会把文件大小计入内存响应队列；响应长度取打开句柄的初始大小。Windows 日志下载允许写入和轮转，截断会表现为短传输。

## 常规回归与试点退役

所有保留的覆盖进入 `BUILD_TESTS=ON` 的常规测试，无额外试点开关：

| 原试点验证 | 常规覆盖 |
| --- | --- |
| 成功 SSE、上游错误/空正文、结束块 | `HttpSafety` / `AiStreamSafety`；真实 curl 上游和 TLS 报文 |
| 大于 16 KiB 的后端块及嵌入 NUL | `AiStreamTransport.SplitsLargeBackendChunksWithoutLosingBinaryBytes`；65,537 字节数据、32 KiB 队列及完整结束块 |
| 单 I/O 线程、容量耗尽与释放 | `AiStreamTransport.BoundedStreamsKeepOneIoThreadResponsiveAndReleaseCapacity`；503/Retry-After、无额外上游请求、释放后重新成功接纳 |
| 慢客户端背压 | `AiStreamTransport.SlowClientTimesOutWithoutQueuingUnboundedChunks` |
| 停机取消静默上游及回收线程 | `AiStreamTransport.ShutdownCancelsIdleUpstreamAndJoinsWorkersBeforeStoppingListener` 与 `AiStreamShutdown` |
| 容量参数 | `AiStreamExecutor.RejectsInvalidCapacity` |
| 登录、Origin、JSON、预检及路由分发 | `ConfigHttpRoutes`；正式 Web UI 处理器和真实 TLS 路由矩阵 |
| 请求体/头上限、歧义 framing、TLS/配对身份 | `BeastHttp`；包含缺失或不可信客户端证书、CA 和主机名验证 |
| 首包前断连、HTTP/1.0 | `HttpSafety` 的正式 SSE 用例 |

环回专用绑定和试点 Bearer 授权属于已删除工具的策略，不迁入产品认证。证书授权由正式 CA / 配对策略测试覆盖。原试点与正式实现的对照测试共享同一传输，退役后由正式报文结果断言覆盖。

本地验证命令（沿用已有构建目录和依赖缓存）：

```text
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

`beast_http_unit_tests`、`http_safety_unit_tests`、`webhook_transport_tests` 和主测试中的 `ConfigHttpRoutes` / 配对策略回归共同覆盖迁移边界。Windows 本地通过不代表其他平台或物理 Moonlight 客户端已验收；跨平台结果以对应 CI 为准。

## 构建配置与共享目标

项目及 Presets 使用 CMake 3.25 基线，应用目标明确要求 C++23。Debug 断言使用 CMake 的配置级默认值，Windows `nvhttp.cpp` 的 Debug 优化使用生成器表达式，因此 Ninja 单配置与 Multi-Config 的行为一致。

`sunshine::tls` / `sunshine::curl` 统一网络依赖；MinGW 在依赖发现时选择静态 OpenSSL，并迁移旧缓存中的 DLL 导入库选择。应用专用的 FFmpeg 和平台 include 位于 `sunshine_build_dependencies`，不进入共用 HTTP 目标。Windows 静态链接选项应用于项目自己的目标；资源文件由独立 object target 编译，避免把整个 C++ include 图传给 windres。

`sunshine_crypto`、`sunshine_ai_provider`、`sunshine_ai_stream` 复用没有 `SUNSHINE_TESTS` 实现变体的源码。应用与独立测试在目标声明处显式链接这些共享库，不扫描或改写其他目标的源码；日志、输入、编码后端和 Webhook 运行时保留各自测试变体。开启 coverage 时，共享模块与测试均参与插桩，不通过全局 C/C++ flags 改写第三方目标。

Web UI 使用锁文件安装依赖，安装和打包各有输出与输入依赖：源文件变化重建页面，锁文件变化重装依赖，缺失输出触发恢复，无改动构建跳过 npm。资源复制保留子目录，并跟踪编辑、新增和删除；仅清理规则自己记录的输出。Wayland 的两个生成命令位于构建图中，XML 变化触发重建，首个命令失败直接结束构建。

`cmake_configuration_tests` 保留六组行为回归：Preset 构建目录隔离、Debug 断言、系统 Boost 组件、Web UI 增量构建、资源复制与 Wayland 生成。Debug 探针直接使用生产构建规则，发现 `NDEBUG` 时编译失败，无需截取 CMake 文本。Web UI/npm 和 Wayland/scanner 使用可记录调用的本地替身，无需下载依赖或安装 Wayland。

Windows `test-win` Preset 使用独立的 `build-tests` 目录，避免覆盖 `dev-win` 构建的 `build/tests` 子目录。

公共宿主文件在 `cmake/sources/host.cmake` 中按功能分组、以短路径显式列举，相关 `.cpp/.h` 成对排列；头文件作为私有源文件保留 IDE 展示，不改变 include 顺序。`main.cpp` 只在应用目标中声明，主测试直接使用公共源码及平台清单，保持自己的 `SUNSHINE_TESTS` 编译定义；三个共享库继续拥有各自的实现文件。
