# Windows 双端目录映射补强记录

本文是 `docs/windows_directory_mapping_design.md` 的补充记录，用于固化正式实施前必须纳入的可靠性、安全性和产品化补强点。

## 结论

当前“Moonlight 主动建立双向 WSS 文件 RPC，双端只暴露受控 mapping，后续可选接入 WinFsp 虚拟盘”的主方案方向可靠，也符合远控产品的成熟演进路径。

但正式落地时，不能只实现裸 `list/read/write`。需要把传输协议、任务模型、QoS、授权、审计和 Windows 竞态安全一起纳入设计基线。

## 必须补强项

### 1. 控制消息与文件数据分离

控制消息可以继续使用 JSON：

```text
hello
list
stat
open
close
mkdir
rename
delete
job_start
job_status
cancel
error
```

文件块数据不建议长期使用 JSON base64。最终协议应使用 WebSocket binary frame。

原因：

- base64 体积约增加三分之一。
- 大文件传输会产生额外 CPU 和内存拷贝。
- binary frame 更适合做背压、限速和零拷贝优化。

建议：

```text
JSON frame: 控制面
Binary frame: 数据面
```

binary frame 头部至少包含：

```text
protocol_version
request_id
job_id
handle_id
offset
flags
payload_length
```

### 2. 引入 Transfer Job 模型

用户可见的上传、下载、复制不应直接暴露为一组裸 RPC，而应抽象为 transfer job。

示例：

```json
{
  "type": "job_start",
  "id": 200,
  "job_id": "job-abc",
  "operation": "download",
  "source": {
    "side": "host",
    "mapping": "host-downloads",
    "path": "games/setup.exe"
  },
  "destination": {
    "side": "client",
    "mapping": "client-downloads",
    "path": "setup.exe"
  }
}
```

job 状态：

```text
queued
running
paused
completed
failed
cancelled
```

每个 job 至少记录：

```text
job_id
operation
source
destination
total_bytes
transferred_bytes
state
error_code
error_message
created_at
updated_at
optional_hash_or_etag
```

收益：

- UI 可以稳定显示进度。
- 用户可以取消、暂停、重试。
- helper 崩溃或重连后，任务不会误显示为完成。
- 后续可以支持队列、限速、批量传输和断点续传。

### 3. 文件传输必须有 QoS

文件传输必须是串流会话中的低优先级后台负载，不能影响视频、音频和输入。

建议策略：

```text
局域网默认上限: 不限或 200 Mbps
远程网络默认上限: 20 Mbps
弱网状态: 降到 5 Mbps 或暂停后台传输
用户前台主动传输: 可临时提高上限，但仍不能阻塞输入/control
```

触发降速的信号：

- RTT 升高。
- 丢包升高。
- 视频码率被 ABR 下调。
- 解码帧率下降。
- 输入延迟升高。
- WSS send queue 持续积压。

实现要求：

- 文件传输队列与串流主循环解耦。
- 文件传输不得在 Session 主线程执行阻塞 I/O。
- 支持全局限速和单 job 限速。
- 支持 `cancel`，取消必须尽快生效。

### 4. 双向共享必须显式授权

Sunshine 共享目录给 Moonlight：

- 在 Sunshine Web UI 中配置。
- 按客户端 UUID 授权。
- 默认只读。

Moonlight 共享目录给 Sunshine：

- 在 moonlight-qt 中配置。
- 按主机授权。
- 首次访问建议弹窗确认。

首次访问确认选项：

```text
本次允许
始终允许此主机
拒绝
```

高风险能力必须额外确认：

```text
readwrite
delete
rename
replace
bulk transfer
execute
```

`allow_execute` 默认必须关闭。未来如果支持远端请求执行文件，应作为独立高危能力，不应混在普通目录映射里。

### 5. 加强 Windows 路径与 TOCTOU 防护

已有规则：

- 远端只能传相对路径。
- 使用 `mapping_id + relative_path`。
- canonicalize 后必须仍在 `local_root` 下。
- 默认拒绝 junction、symlink、mount point 等 reparse point。

需要补强：

- 不只在 open 前检查路径。
- 打开文件后也要基于 handle 复核属性。
- rename/delete/replace 要同时校验源路径和目标路径。
- 目录枚举结果不能被后续操作无条件信任。

典型 TOCTOU 风险：

```text
1. 远端请求 safe/file.txt
2. 本地检查时路径位于授权目录内
3. 本地其他进程把 safe 替换成 junction
4. 实际 I/O 逃逸到授权目录外
```

建议措施：

- 优先使用 Windows handle-based API。
- `CreateFileW` 后调用 `GetFileInformationByHandleEx` 复核属性。
- 对 `FILE_ATTRIBUTE_REPARSE_POINT` 默认拒绝。
- 写入使用同目录临时文件，完成后原子 rename。
- delete/rename 前后都进行最终路径校验。
- 对目录 handle 和文件 handle 保持最小必要权限。

### 6. 增加审计日志

两端都应记录安全相关事件：

```text
mapping created
mapping updated
mapping removed
file channel connected
file channel disconnected
list
read
write
mkdir
rename
delete
job started
job completed
job failed
job cancelled
access denied
path escape blocked
reparse point blocked
```

日志字段：

```text
timestamp
local_side
remote_uuid
remote_name
mapping_id
operation
relative_path
result
error_code
bytes_transferred
job_id
```

禁止写入日志：

```text
client private key
raw auth token
full certificate secret material
unmasked credentials
```

### 7. 虚拟盘阶段必须单独定义边界

WinFsp/Dokany 是第三阶段能力，不应影响第一版文件面板交付。

虚拟盘阶段需要单独定义：

- 挂载前用户确认，明确说明本机其他程序也能读取挂载目录。
- 挂载生命周期：Moonlight 退出、串流断开、网络失败、主机撤销共享时自动卸载。
- 只读挂载的 Explorer 行为：缩略图、索引器、预览器和普通程序读请求都必须受同一套限流与路径策略约束。
- 缓存策略。
- 文件锁语义。
- 一致性模型。
- rename/replace 行为。
- 大量小文件性能。
- 随机 I/O 性能。
- 是否允许从远端盘执行程序。
- 是否允许游戏或大型应用直接运行。

第一版不应承诺：

```text
从远端虚拟盘直接运行游戏
从远端虚拟盘直接运行大型程序
完全等价于本地 NTFS 盘
完全等价于 SMB/RDP drive redirection
```

## 推荐实施基线

Phase 1 必须包含：

- JSON 控制消息。
- binary 文件块，或至少预留 binary frame 升级点。
- transfer job。
- 基础限速。
- 显式授权。
- 路径逃逸防护。
- reparse point 默认拒绝。
- 审计日志。

Phase 2 必须包含：

- Moonlight 客户端目录反向共享。
- Sunshine 通过同一条 WSS 访问客户端 mapping。
- 首次访问确认。
- 双端统一 job 状态和错误码。

Phase 3 才考虑：

- WinFsp/Dokany。
- 只读盘符或目录挂载。
- 缓存和文件锁。
- 虚拟盘性能边界。

## 最终判断

主方案先进且可行。它比 SMB 更适合 Sunshine/Moonlight 的连接模型，比把数据塞进 Limelight control stream 更可靠，也比第一版直接做虚拟盘更稳。

正式实现时，只要把本文补强项纳入基线，就可以作为一个安全、可演进、产品体验足够好的双端目录映射方案推进。

## WSS 实现补充

`Simple-Web-Server` 不应承担完整文件映射 WebSocket 数据通道。

保留它处理：

- capability endpoint
- Web UI 管理接口
- 配置保存
- 健康检查

真正的文件通道应使用 Boost.Beast：

- Beast 已基于 Boost.Asio，和 Sunshine 当前依赖栈契合。
- Beast 原生支持 WebSocket frame、binary/text、ping/pong、close、read/write 异步模型。
- 可以更自然地实现 backpressure、限速和 session 生命周期。

实施基线：

```text
file_mapping_http: REST/discovery/config
file_mapping_ws: Beast WebSocket transport
file_mapping_rpc: protocol model
file_mapping: local filesystem safety
file_mapping::service_t: built-in feature lifecycle and capability state
```

除非后续统一网络层，否则不要在 `Simple-Web-Server::on_upgrade` 上手写完整 WebSocket 协议。

## 当前落地状态记录

Sunshine 侧已拆出 `src/file_mapping/` 内置 feature module。`file_mapping::service_t` 负责配置解析、mapping store 注入、token store、Boost.Asio/Boost.Beast WSS listener 生命周期和 capability 状态；`nvhttp::start()` 只负责启动 service 并把 GameStream HTTPS capability endpoint 转发给它。能力发现 endpoint 已从 Web UI 配置 HTTPS 端口迁移到 GameStream `nvhttp` HTTPS 端口，复用 paired client certificate 校验。

当前 capability 接口返回：

- `enabled`：Sunshine 是否编译并尝试启用文件映射能力。
- `listening`：Beast WSS listener 是否已经监听。
- `port`：本次启动绑定的动态端口。
- `session_url`：只在 Sunshine 内部有可信显式地址时返回；不得根据当前 HTTPS 请求 `Host` 头拼接。
- `session_token`：短期一次性 WSS 连接 token。
- `client_uuid`：Sunshine 从 HTTPS 客户端证书反查 pairing store 得到的内部配对证书 UUID。
- `error`：WSS listener 启动失败原因。

当前已补的安全点：

- capability 每次返回短期一次性 `session_token`。
- capability 请求可以携带 Moonlight `IdentityManager::getUniqueId()` 作为 query/header 诊断 hint，但 Sunshine 不把该值作为授权依据。
- capability 通过 `get_client_cert_uuid_from_request()` 从 nvhttp HTTPS 客户端证书推导 Sunshine pairing store 内部 UUID。
- `session_token` 已绑定证书推导出的 `client_uuid`。
- 默认 capability 只返回 `port`、`session_endpoint` 和 `session_token`。客户端使用当前已连接的 Sunshine 主机地址自行组合 WSS 目标，并避免记录拼接 token 后的完整 URL。
- Beast WSS 在 WebSocket upgrade 前读取 HTTP request target，并消费 token。
- token 缺失、过期、错误、重放都会拒绝进入文件映射协议。
- Moonlight `hello.client_uuid` 必须使用 capability 返回的 `client_uuid`，并和 token 绑定 UUID 一致。
- Web UI / control-panel 持久化 `file_mappings` 时写入 `base64:<json>`；解析层继续兼容旧 raw JSON array。这样可以避免 `sunshine.conf` 行解析器把 JSON 字符串里的 `]`、`#` 或换行当作配置语法。
- `hello.client_uuid` 必须存在于 Sunshine 已配对客户端列表。

当前已补的文件能力：

- 已新增 `file_mappings` 配置项，使用 JSON array 注入 host mappings。
- `file_mapping::service_t` 会解析 `config::nvhttp.file_mappings` 并注入 WSS server 的 operations context。
- 已新增 Sunshine 本机管理 API 和 `file_mapping_store`，支持运行期创建、列出、更新、删除 mapping，并持久化回 `file_mappings`。
- 管理 API 写入采用失败回滚：配置持久化失败时恢复旧 store，避免 HTTP 返回失败但运行态已经生效。
- 第一阶段固定只读：`readwrite`、`allow_delete=true`、`allow_execute=true`、`follow_reparse_points=true` 都不会进入运行时 store。
- WSS 已增加第一批资源闸门：token 全局/单客户端配额、签发间隔、Beast message size limit、active session 上限、write queue 上限。
- 目录 listing 已增加默认返回数量上限，并通过 `truncated=true` 告诉客户端需要分页/继续读取。
- RPC 已增加最小 job model：`list`、`stat`、`read` 回包携带 `job_id` / `job`，并支持 `job_status` 与 `cancel_job` 查询/取消入口。
- capability response 已增加 diagnostics，明确返回 bind address、configured/bound port、listening、client certificate、token issue/rate-limit 等状态。
- 已新增只读 RPC 执行器 `file_mapping_operations`。
- 已支持 host mapping 的 `list`、`stat`、`read`。
- `read` 当前返回 JSON `result`，文件数据使用 base64 编码。
- `session_core_t` 已在 hello 后调用真实只读执行器，不再返回占位 `handled`。
- mapping 白名单会按 `peer_uuid` 校验 `mapping.clients`。

Moonlight-qt 侧当前已补：

- 已新增 `app/streaming/FileMappingClient` 原型模块，挂入 `app.pro`。
- capability 请求会携带 Moonlight `IdentityManager::getUniqueId()` 作为 `client_uuid` query 和 `X-File-Mapping-Client-UUID` 头。
- capability 响应会解析 Sunshine 返回的 `client_uuid`；WSS `hello.client_uuid` 优先使用该值，避免误用 Moonlight uniqueid 或 Sunshine host uuid。
- 复用现有 `IdentityManager::getSslConfig()` 和 Sunshine 已配对服务器证书 pinning。
- 当前 Qt 安装缺少 `QtWebSockets` 模块，因此客户端先使用 `QSslSocket` 实现最小 WSS：TLS、HTTP Upgrade、`Sec-WebSocket-Accept` 校验、client masked text frame、server text frame JSON 解析。
- 已提供 `smokeRead()`，可按 `capability -> WSS -> hello -> list -> read` 跑第一阶段端到端烟测。
- `FileMappingClient` 已改为使用 Moonlight 客户端 `IdentityManager::getUniqueId()` 作为 capability hint；`NvComputer::uuid` 是 Sunshine 主机 UUID，不能用于 WSS token 绑定。
- 已在 `Session` 连接成功后接入隐藏烟测开关：`MOONLIGHT_FILE_MAPPING_SMOKE=1`，并通过 `MOONLIGHT_FILE_MAPPING_SMOKE_MAPPING` / `MOONLIGHT_FILE_MAPPING_SMOKE_PATH` 指定测试目标。
- 烟测任务在线程池中后台执行，不阻塞普通串流启动；默认关闭。
- 烟测任务会在 `NvComputer::lock` 读锁保护下复制主机快照，再交给后台线程，避免运行时和主机状态刷新竞态。
- capability 请求先读取 response body 再释放 `QNetworkReply`，避免 worker 线程里对象释放顺序依赖 `deleteLater()` 的隐式时机。
- capability 失败会记录 HTTP status 和响应 body 摘要；RPC `type:error` 会直接输出服务端 `message/code`，异常响应会记录 `hello/list/read` 三段 compact JSON，便于真实端到端冒烟时定位失败点。
- 已新增独立 smoke CLI 子项目 `filemapping-smoke`，不依赖 Qt Multimedia 或完整 UI，可直接命令行验证 `capability -> WSS -> hello -> list -> read`。

当前文件能力仍未完成：

- Sunshine Web UI 还没有目录 mapping 管理页面。
- Control Panel 目前只覆盖普通用户快速共享、列表和移除；高级权限 UI 还未完整开放。
- WSS 仍需要接入 paired client certificate verify callback；当前 Beast 端口仍主要依赖 capability 签发的一次性 token。
- 文件打开仍需要补 Windows handle-level final path 校验，消除 resolve 后到 open/read 之间的 TOCTOU 窗口。
- WSS 仍需要补 handshake/idle timeout、ping/pong、异步任务中途取消和更完整的审计日志。
- 当前 job model 仍是同步执行后的状态记录；长耗时目录遍历/文件传输要支持真正中途取消，还需要把 executor 改成异步 job runner。
- 大文件数据还没有切换到 WebSocket binary frame。
- 写入、删除、rename、mkdir 尚未启用。
- Moonlight-qt 文件面板、helper 进程隔离、反向客户端目录共享尚未接入。
- Moonlight-qt 当前 WSS 实现只覆盖首轮烟测；产品化前仍需补 close handshake、ping/pong、fragment、binary frame、backpressure 和任务取消。

当前验证状态：

- 已完成模块级语法编译：core path、RPC、token、HTTP、operations、WS core、WS session、WS server、config parser。
- 已完成运行冒烟：配置 JSON -> mapping -> session `hello` -> `read`。
- 已新增 GTest 网络级冒烟：`test_file_mapping_ws_network.cpp` 会启动真实 WSS server，并用 Beast SSL/WebSocket client 完成 `hello` -> `list` -> `read`。
- 当前机器完整 CMake/GTest 被 MSYS2 UCRT 编译器构建 Boost 源码失败挡住，失败发生在 Boost `alloc_lib.c` / `dlmalloc.cpp`，尚未进入 Sunshine 新增代码。
- 当前机器独立 WSS 网络探针使用 Strawberry g++ 链接 MSYS OpenSSL 后在进入 `main()` 前以 `0xC00000FD` 退出，判断为运行库混链问题；网络级源码已完成语法编译，待正常统一工具链执行。
- Moonlight-qt 已完成 qmake 验证：使用 `CONFIG+=config_SL` 避开当前机器缺失的 Qt Multimedia 模块后，`app.pro` 可生成 MSVC Makefile。
- Moonlight-qt 已完成 MSVC 最小编译验证：`debug\filemappingclient.obj`、`debug\moc_filemappingclient.cpp`、`debug\moc_filemappingclient.obj` 均编译通过。
- Moonlight-qt 已完成 Session 接入编译验证：在当前机器缺少 Steam Link SDK 头文件的情况下，使用临时 build 目录占位 `SLVideo.h` 后，`debug\session.obj` 编译通过，用于证明新增 `Session` 接线、主机快照加锁复制和后台烟测任务没有 C++ 编译错误。
- Moonlight-qt 独立 smoke CLI 已完成 qmake + MSVC 编译链接验证，`--help` 和缺参 usage 自检可正常退出。

当前刻意保留的安全取舍：

- Beast WSS 目前复用 Sunshine 服务器证书，只做服务器 TLS。
- paired Moonlight 客户端证书校验尚未直接接入 Beast `ssl::context`。
- 第一阶段的强认证发生在 `nvhttp` capability 请求：只有通过 GameStream HTTPS paired client certificate 校验的客户端才能拿到一次性 token。
- Beast WSS 端口依赖短期一次性 token、token 绑定 UUID、hello UUID 一致性校验和 pairing store 白名单。该模型已经比 Web UI BasicAuth 方案更适合 Moonlight 真实链路，但仍可进一步升级为 Beast 端也直接校验证书。

因此下一步必须补：

1. 增加 WSS auth/audit 日志，记录 token/UUID/证书失败原因但不回显给网络端。
2. 对 capability token 签发增加速率限制和失败计数。
3. 评估是否将 Sunshine pairing store 中的客户端证书接入 Beast verify callback，使 WSS 端口也具备 mTLS 双因子防线。
4. 将真实 `file_mapping_smoke=passed` 写入验收记录后，再推进文件面板和 binary frame。
