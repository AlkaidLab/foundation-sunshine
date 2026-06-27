# Windows 双端目录映射实施方案

## 目标

在 Sunshine 与自研 moonlight-qt 均运行于 Windows 的前提下，实现会话级双端目录映射：

- Moonlight 可以访问 Sunshine 主机授权的目录。
- Sunshine 可以访问 Moonlight 客户端授权的目录。
- 第一阶段提供稳定的文件浏览和下载能力，默认只读。
- 后续可选接入 WinFsp/Dokany，将远端目录挂载为 Windows 盘符或目录。

本方案不把目录映射设计成 SMB 或驱动级共享，而是设计为基于现有配对身份的双向文件 RPC 通道。虚拟盘只是 RPC 的一个前端，不是第一版核心。

## 设计原则

- 远端永远不能直接提交本机绝对路径。
- 文件访问统一使用 `mapping_id + relative_path`。
- 拥有本地文件系统的一端负责路径解析、权限判断和实际 I/O。
- 文件传输不进入视频、音频、输入、Limelight control stream 热路径。
- Moonlight 作为主动连接方建立双向通道，避免 NAT 和客户端防火墙问题。
- 复用现有配对证书和会话生命周期，不引入额外账号体系。
- 第一版优先可靠、安全、可调试，再考虑虚拟盘体验。

## 总体架构

```text
Windows Explorer
  右键目录
    |
Rust GUI / Control Panel
  快速共享入口
  映射管理 UI
  通知和审计展示
  本地管理 API 调用
    |
Sunshine core                                  moonlight-qt
-------------                                  ------------
file_mapping_core                              file-mapping-helper
  本地目录授权                                    本地目录授权
  路径解析/权限检查                                路径解析/权限检查
  文件读写                                        文件读写
        \                                      /
         \                                    /
          WSS/HTTPS bidirectional file RPC
         /                                    \
        /                                      \
WinFsp/Dokany mount optional            WinFsp/Dokany mount optional
```

### 分层边界

Rust GUI / Control Panel 是用户体验与配置编排层：

- 注册或移除 Explorer 目录右键菜单。
- 接收 `--quick-share-folder <path>`。
- 做本机路径存在性预检。
- 调 Sunshine 本地管理 API 创建安全默认 mapping。
- 展示共享列表、高级权限设置、撤销入口和审计摘要。
- 发送系统通知。

Sunshine core 是安全边界和数据面：

- 持久化 mapping 配置。
- 最终校验路径、权限、reparse point、设备授权。
- 负责 paired client certificate、capability token、WSS hello UUID 一致性。
- 执行文件 I/O 和 WSS RPC。
- 记录审计事件。

Rust GUI 的校验只能提升体验，不能替代 core 校验。任何来自右键菜单、Web UI、配置文件或本地 API 的 mapping 都必须经过 core 的同一套安全规则。

### Sunshine 侧模块

建议新增：

```text
src/file_mapping/
  file_mapping.h
  file_mapping.cpp
  file_mapping_rpc.h
  file_mapping_rpc.cpp
  file_mapping_http.h
  file_mapping_http.cpp
  service.h
  service.cpp
```

职责：

- `file_mapping_core`：配置、权限、路径解析、文件 I/O。
- `file_mapping_rpc`：请求/响应模型、handle 管理、流控、错误码。
- `file_mapping_http`：HTTPS/WSS 路由注册，复用 Sunshine 现有证书与客户端认证。
- `file_mapping_config`：mapping JSON 解析、默认值归一化、配置错误报告。
- `file_mapping::service_t`：内置 feature service，负责 WSS 生命周期、token store、capability 状态和 mapping store 注入。

路由注册方式建议参考 `clipboard_http::register_routes()`，避免继续膨胀 `confighttp.cpp`。

后续为了支持右键快速共享，还需要新增本地管理 API：

```text
POST /api/v1/file-mapping/mappings
GET /api/v1/file-mapping/mappings
PATCH /api/v1/file-mapping/mappings/{id}
DELETE /api/v1/file-mapping/mappings/{id}
```

这些 API 只服务本机 Control Panel / Rust GUI，不走 GameStream `nvhttp`。它们负责配置管理，不负责文件数据传输。

Beast WSS 数据面默认监听 `file_mapping_port = 48010`。该端口应允许通过配置或命令行覆盖，便于多实例测试、端口冲突规避和受管环境部署；capability 响应必须返回实际监听端口。

### moonlight-qt 侧模块

建议新增：

```text
app/streaming/filemappinghelperclient.h
app/streaming/filemappinghelperclient.cpp
app/streaming/filemappingipc.h
app/streaming/filemappingipc.cpp
file-mapping-helper/
```

职责：

- `Session` 管理 helper 生命周期。
- `FileMappingHelperClient` 负责启动、停止、重启 helper，并通过 IPC 下发 host 地址、HTTPS 端口、证书、客户端私钥、本地目录配置。
- `file-mapping-helper` 负责本地文件系统访问、WSS 连接、双向 RPC 处理。

该结构可直接复用现有 `ClipboardHelperClient`、`ClipboardIpc`、`ClipboardSync` 的工程模式。

## 能力发现

Sunshine 在 `serverinfo` 中暴露能力：

```xml
<sunshineCapabilities>
  <fileMapping>1</fileMapping>
  <fileMappingVersion>1</fileMappingVersion>
</sunshineCapabilities>
```

moonlight-qt 在 `NvHTTP::getServerInfo()` 后解析能力。仅当目标为 Sunshine 且能力存在时启用目录映射。NVIDIA GFE 路径保持不变。

## 映射配置模型

```json
{
  "id": "host-downloads",
  "name": "Downloads",
  "side": "host",
  "local_root": "D:\\Downloads",
  "mode": "read",
  "allow_delete": false,
  "allow_execute": false,
  "follow_reparse_points": false,
  "clients": ["client_uuid"],
  "max_file_size": 10737418240
}
```

字段说明：

- `id`：稳定映射 ID，只能包含字母、数字、`_`、`-`。
- `name`：显示名称。
- `side`：`host` 或 `client`。
- `local_root`：本机真实目录，只保存在拥有该目录的一端。
- `mode`：第一阶段只允许 `read`。配置或管理 API 传入 `readwrite` 时，core 必须拒绝或降级为只读。
- `allow_delete`：第一阶段必须保持 `false`，不开放远端删除。
- `allow_execute`：必须保持 `false`，不开放远端执行。
- `follow_reparse_points`：必须保持 `false`，不允许穿透 junction、symlink、mount point。
- `clients`：允许访问的配对客户端 UUID。
- `max_file_size`：单文件操作上限。

远端看到的是：

```json
{
  "id": "host-downloads",
  "name": "Downloads",
  "side": "host",
  "mode": "read",
  "capabilities": ["list", "read"]
}
```

远端不应看到 `local_root`。

## 连接流程

1. Moonlight 与 Sunshine 完成配对。
2. Moonlight 获取 `serverinfo`，发现 `fileMappingVersion=1`。
3. 用户启动串流。
4. `Session` 创建 `FileMappingHelperClient`。
5. `FileMappingHelperClient` 启动 `moonlight-file-mapping-helper.exe`。
6. 主进程通过 IPC 将 host 地址、HTTPS 端口、服务端证书、客户端证书、客户端私钥和本地映射配置传给 helper。
7. helper 主动连接：

```text
wss://<sunshine-host>:<https-port>/api/v1/file-mapping/session
```

8. Sunshine 使用已有客户端证书识别客户端 UUID。
9. 双方交换 `hello` 消息，声明协议版本和各自可共享的映射。
10. 文件操作均通过该双向 WSS 通道完成。

## RPC 消息模型

### Hello

```json
{
  "type": "hello",
  "version": 1,
  "endpoint": "client",
  "client_uuid": "...",
  "mappings": [
    {
      "id": "client-docs",
      "name": "Documents",
      "side": "client",
      "mode": "read"
    }
  ]
}
```

### List

```json
{
  "type": "list",
  "id": 100,
  "mapping": "host-downloads",
  "path": "games/"
}
```

响应：

```json
{
  "type": "result",
  "id": 100,
  "entries": [
    {
      "name": "setup.exe",
      "kind": "file",
      "size": 123456,
      "mtime": 1782345678
    }
  ]
}
```

### Stat

```json
{
  "type": "stat",
  "id": 101,
  "mapping": "host-downloads",
  "path": "games/setup.exe"
}
```

### Open

```json
{
  "type": "open",
  "id": 102,
  "mapping": "host-downloads",
  "path": "games/setup.exe",
  "mode": "read"
}
```

响应：

```json
{
  "type": "result",
  "id": 102,
  "handle": "h-123",
  "size": 123456
}
```

### Read

```json
{
  "type": "read",
  "id": 103,
  "handle": "h-123",
  "offset": 0,
  "length": 262144
}
```

响应：

```json
{
  "type": "data",
  "id": 103,
  "eof": false,
  "bytes_base64": "..."
}
```

### Current Sunshine read-only RPC

当前 Sunshine 第一版只读执行器已实现：

- `list`：`{"type":"list","id":1,"mapping":"host-test","path":""}`
- `stat`：`{"type":"stat","id":2,"mapping":"host-test","path":"hello.txt"}`
- `read` / `read_chunk`：`{"type":"read","id":3,"mapping":"host-test","path":"hello.txt","offset":0,"length":65536}`
- `open`：当前 read-only executor 不提供 handle 语义，必须返回 `unsupported_operation`，不能隐式路由到 `read`。

当前 `read` 响应暂时使用 JSON + base64：

```json
{
  "type": "result",
  "id": 3,
  "ok": true,
  "mapping": "host-test",
  "path": "hello.txt",
  "offset": 0,
  "bytes_read": 11,
  "total_size": 11,
  "eof": true,
  "encoding": "base64",
  "data": "aGVsbG8gd29ybGQ="
}
```

这用于第一阶段冒烟和 Moonlight 侧 UI/流程打通。大文件传输仍应按设计升级为 WebSocket binary frame，避免长期使用 JSON base64 承载大块数据。

### Current moonlight-qt smoke client

当前 moonlight-qt 已新增 `app/streaming/FileMappingClient` 原型，用于打通第一阶段 `capability -> WSS -> hello -> list -> read`：

- `fetchCapability()` 请求 Sunshine GameStream HTTPS 端口上的 `GET /api/v1/file-mapping/capability`，即 `NvComputer::activeHttpsPort`，通常是 `47984`，不是 Web UI 配置端口。
- 请求仍携带 Moonlight `IdentityManager::getUniqueId()` 作为 `client_uuid` query 和 `X-File-Mapping-Client-UUID` header，主要用于诊断和兼容；Sunshine 授权以 HTTPS 客户端证书推导出的配对 UUID 为准。
- Sunshine capability 响应会返回 `client_uuid`，这是 Sunshine pairing store 内部的配对证书 UUID。
- `smokeRead()` 使用 capability 返回的 `session_url` 和独立 `session_token` 建立 WSS；实现中需要避免记录拼接 token 后的完整 URL。`hello.client_uuid` 使用 capability 返回的 `client_uuid`，然后发送 `list`、`read`。
- 该模块复用 Moonlight 现有客户端证书配置和 Sunshine 服务端证书 pinning。
- 当前开发环境没有 QtWebSockets 模块，因此该原型使用 `QSslSocket` 实现最小 WebSocket handshake 和 text frame 收发；这只作为烟测客户端，不替代后续完整产品化传输层。

当前 `Session` 已接入隐藏烟测开关。启动 moonlight-qt 前设置：

```powershell
$env:MOONLIGHT_FILE_MAPPING_SMOKE="1"
$env:MOONLIGHT_FILE_MAPPING_SMOKE_MAPPING="host-test"
$env:MOONLIGHT_FILE_MAPPING_SMOKE_PATH="hello.txt"
```

可选参数：

```powershell
$env:MOONLIGHT_FILE_MAPPING_SMOKE_TIMEOUT_MS="5000"
$env:MOONLIGHT_FILE_MAPPING_SMOKE_OFFSET="0"
$env:MOONLIGHT_FILE_MAPPING_SMOKE_LENGTH="4096"
```

串流连接成功后，moonlight-qt 会在线程池中后台执行一次 `smokeRead()`，并在日志中输出 `File mapping smoke passed` 或失败原因。该开关默认关闭，不影响普通串流路径。

为了不依赖完整串流 UI，moonlight-qt 还提供独立 smoke CLI：

```powershell
cd C:\Users\mohaha\source\repos\moonlight-qt
mkdir build-filemapping-smoke
cd build-filemapping-smoke
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 && C:\Qt\6.8.2\msvc2022_64\bin\qmake.exe ..\filemapping-smoke\filemapping-smoke.pro -spec win32-msvc CONFIG+=debug && nmake'
```

运行：

```powershell
$env:PATH="C:\Qt\6.8.2\msvc2022_64\bin;C:\Users\mohaha\source\repos\moonlight-qt\libs\windows\lib\x64;$env:PATH"
.\debug\moonlight-filemapping-smoke.exe `
  --host 127.0.0.1 `
  --https-port 47984 `
 --server-cert C:\path\to\sunshine-server.pem `
  --mapping host-test `
  --path hello.txt
```

该工具会打印 `client_uuid=<Moonlight uniqueid>`，并使用同一个 `FileMappingClient` 执行 `capability -> WSS -> hello -> list -> read`。注意 `--https-port` 应使用 GameStream HTTPS 端口，通常是 `47984`；`NvComputer::uuid` 是 Sunshine 主机 UUID，不能用于 WSS token/hello 绑定。实际 WSS `hello.client_uuid` 使用 Sunshine capability 返回的证书配对 UUID。

### Write

```json
{
  "type": "write",
  "id": 104,
  "handle": "h-456",
  "offset": 0,
  "bytes_base64": "..."
}
```

### Close

```json
{
  "type": "close",
  "id": 105,
  "handle": "h-123"
}
```

### Error

```json
{
  "type": "error",
  "id": 103,
  "code": "access_denied",
  "message": "Access denied"
}
```

建议错误码：

```text
bad_request
unsupported_version
not_authenticated
access_denied
mapping_not_found
path_escape
not_found
already_exists
not_directory
is_directory
file_too_large
read_only
reparse_point_blocked
io_error
cancelled
rate_limited
```

## 路径安全规则

Windows 路径处理必须作为核心安全边界：

- 网络层路径统一使用 UTF-8 和 `/`。
- 本地文件访问统一转换为 UTF-16 Windows API。
- 请求路径必须是相对路径。
- 禁止远端传入盘符、UNC 路径、设备路径。
- 禁止 `..` 逃逸。
- `local_root + relative_path` 后必须 canonicalize。
- canonicalize 结果必须仍位于 `local_root` 下。
- Windows 路径比较按大小写不敏感处理。
- 默认拒绝 `FILE_ATTRIBUTE_REPARSE_POINT`。
- 默认拒绝系统目录作为共享根，例如 `C:\Windows`、`C:\Program Files`。
- 默认拒绝 Windows 保留设备名，例如 `CON`、`PRN`、`AUX`、`NUL`、`COM1`、`LPT1`。
- 写入必须先进入临时文件，完成后原子 rename。

## 性能与可靠性

第一版建议：

- 分块大小默认 256 KiB 或 1 MiB。
- 单连接并发请求数限制为 4 到 8。
- 单映射打开 handle 数限制为 32。
- 支持 request id，用于响应匹配。
- 支持 `cancel` 消息取消长任务。
- 支持心跳与空闲超时。
- 大文件传输必须可恢复到明确错误状态，不允许半写文件冒充完成文件。

后续增强：

- Range 风格断点续传。
- 文件 hash 校验。
- 目录 watch。
- 传输限速。
- 压缩策略。
- mmap 或零拷贝优化。

## UI 方案

### Sunshine Web UI

新增 “Directory Mapping” 配置页：

- 启用目录映射开关。
- 映射列表。
- 添加本地目录。
- 权限选择：只读、读写、允许删除。
- 客户端授权范围。
- 高风险操作提示。

### moonlight-qt UI

新增客户端设置：

- 启用目录映射。
- 选择要分享给主机的本地目录。
- 每个主机独立授权。
- 串流中显示远端文件入口。

第一版 moonlight-qt 先做文件面板，不把盘符挂载作为默认入口。

## 虚拟盘扩展

桌面端 moonlight-qt 可以通过 WinFsp/Dokany 把远端共享目录挂载到 Windows Explorer。该能力应作为可选增强，而不是第一版 UX 闭环的前置条件。

推荐分层：

```text
moonlight-qt file panel
  -> FileMappingClient
      -> WSS RPC

optional Explorer mount
  -> WinFsp/Dokany adapter
      -> FileMappingClient
          -> WSS RPC
```

不推荐把 WebDAV/SMB 作为默认实现。它们会引入额外认证面、缓存语义和系统服务依赖，也不自然复用 Sunshine/Moonlight 已有的配对身份。

第三阶段可接入 WinFsp：

```text
WinFsp filesystem
  -> FileMappingClient
      -> WSS RPC
          -> Remote FileMappingProvider
```

挂载示例：

```text
M:\Host\Downloads
N:\Client\Documents
```

注意：

- WinFsp 层只负责把 Windows 文件系统回调转成 RPC。
- 权限和路径安全仍在 provider 端执行。
- 虚拟盘失败不应影响串流。
- 未安装 WinFsp 时仍保留文件面板能力。
- 第一阶段只允许只读挂载；上传、删除、rename、replace 需要等写入语义和冲突处理单独设计。
- 挂载生命周期绑定 Moonlight 与对应 Sunshine 主机连接；断线、退出或主机撤销共享时必须自动卸载。
- 挂载后本机任意程序都可能读取该盘符，因此 UI 必须明确提示用户这一点。
- 不承诺固定盘符。默认使用可读挂载名，高级设置再允许用户指定盘符或挂载目录。

## 分阶段实施

### Phase 1：Sunshine 到 Moonlight 的文件访问

目标：

- Sunshine 配置共享目录。
- moonlight-qt 文件面板浏览主机目录。
- 支持下载。
- 第一阶段固定 read-only，不开放上传、删除、执行、reparse point 穿透。

交付：

- Sunshine `file_mapping_core`。
- Sunshine WSS session endpoint。
- moonlight-qt `file-mapping-helper`。
- moonlight-qt 文件面板。

### Phase 2：双向目录映射

目标：

- moonlight-qt 配置客户端共享目录。
- Sunshine 可通过同一条 WSS 访问客户端目录。
- 双端统一使用相同 RPC 和权限模型。

交付：

- client-side provider。
- Sunshine 侧 remote client mapping registry。
- 双向权限 UI。

### Phase 3：Windows 虚拟盘

目标：

- 可选安装 WinFsp。
- 将远端目录只读挂载为盘符或目录。
- 文件浏览器和普通 Windows 程序可访问远端目录。
- 断线、退出、主机撤销共享时自动卸载。

交付：

- Moonlight WinFsp adapter。
- Sunshine WinFsp adapter，可选。
- 挂载状态 UI。
- 挂载/卸载失败的用户可读错误提示。

## 测试清单

核心测试：

- 相对路径正常解析。
- `..` 逃逸被拒绝。
- 盘符路径被拒绝。
- UNC 路径被拒绝。
- junction/symlink 默认被拒绝。
- 只读映射拒绝写入、删除、rename。
- 大文件分块读写正确。
- 中断传输不会留下伪完成文件。
- helper 崩溃后不影响串流，并按限制重启。
- reconnect 后文件通道可重新建立。
- 未启用能力的服务器不显示目录映射入口。

Windows 特定测试：

- 中文路径。
- 超长路径。
- 大小写差异路径。
- 保留设备名。
- 文件被占用。
- 权限不足。
- FAT/NTFS 不同行为。

## 推荐结论

优雅落地方式是：

1. 把目录映射抽象为会话级双向文件 RPC。
2. Moonlight 主动连接 Sunshine，复用配对证书。
3. 双端各自只暴露受控 mapping，不暴露真实绝对路径。
4. 第一版做文件面板和稳定传输。
5. 第二版完成客户端反向共享。
6. 第三版用 WinFsp 提供盘符体验。

这样可以在不污染串流热路径、不引入 SMB 复杂度、不要求客户端开放端口的前提下，逐步获得接近本地盘的使用体验。

## HTTP 与 WSS 实现分工

Sunshine 现有 `Simple-Web-Server` 适合继续承担 Web UI、REST API、GameStream `nvhttp` 能力发现和轻量管理接口，但不应作为完整文件映射 WSS 数据通道的主要实现。

原因：

- 文件映射需要长期双向连接。
- 需要 WebSocket binary frame。
- 需要 ping/pong、close handshake、fragment、mask、backpressure。
- 需要大文件分块、取消、限速和弱网恢复。
- `Simple-Web-Server` 只提供较底层的 `on_upgrade` 接管点，完整 WebSocket 协议仍需自行实现。

因此推荐分工：

```text
Simple-Web-Server
  -> nvhttp HTTPS /api/v1/file-mapping/capability
  -> Web UI /api/v1/file-mapping/config
  -> Web UI 管理接口

Boost.Beast / Boost.Asio
  -> file mapping WebSocket session
  -> JSON control frame
  -> binary data frame
  -> ping/pong, close, backpressure
  -> transfer job lifecycle
```

第一阶段 discovery endpoint 挂在 GameStream `nvhttp` HTTPS 端口上，复用现有 paired client certificate 校验：

```text
GET /api/v1/file-mapping/capability
```

Capability 响应中返回实际 WSS 通道信息：

```json
{
  "ok": true,
  "enabled": true,
  "listening": true,
  "version": 1,
  "transport": "wss",
  "port": 47999,
  "session_endpoint": "/api/v1/file-mapping/session",
  "session_url": "",
  "session_token": "...",
  "client_uuid": "<sunshine-paired-cert-uuid>",
  "implementation": "boost.beast"
}
```

`session_url` 只能来自 Sunshine 内部可信状态，不能使用请求 `Host` header 拼接。通常情况下 capability 返回 `port` 和 `session_endpoint`，Moonlight 使用当前已连接的 Sunshine 主机地址自行组合 WSS 目标，避免 Host header injection。

Moonlight 请求 capability 时可携带自身 `IdentityManager::getUniqueId()` 作为诊断 hint：

```http
GET /api/v1/file-mapping/capability?client_uuid=<moonlight-uniqueid>
```

也可使用请求头：

```http
X-File-Mapping-Client-UUID: <moonlight-uniqueid>
```

Sunshine 不信任请求里的 UUID 作为授权依据，而是从 HTTPS 客户端证书查 pairing store，得到内部 paired cert UUID，并只为该 UUID 签发短期一次性 token。WSS 建连后，Moonlight 的第一条 `hello.client_uuid` 必须使用 capability 返回的 `client_uuid`，并与 token 绑定 UUID 一致。

### Sunshine config entry

第一阶段 Sunshine 通过 `file_mappings` 配置项注入 host mappings。该配置项是 JSON array：

```json
[
  {
    "id": "host-downloads",
    "name": "Downloads",
    "path": "C:/Users/example/Downloads",
    "mode": "read",
    "clients": ["paired-client-uuid"],
    "follow_reparse_points": false,
    "max_file_size": 0
  }
]
```

- `id`：协议内使用的 mapping id，只允许字母、数字、`_`、`-`。
- `path`：Windows 本地目录，必须存在。
- `clients`：为空表示所有已配对客户端可访问；非空时只允许列出的 UUID。
- 第一阶段运行时会跳过无效 mapping，并在日志中写 warning。

实现字段关系：配置持久化和 Web UI API 使用 `path` 表示本机真实目录；运行时 `file_mapping::mapping_t` 使用 `local_root` 保存同一值。协议层和远端响应不得暴露 `local_root`，只暴露 `mapping` id 与相对 `path`。

兼容性：手写配置仍支持上面的 raw JSON array；Web UI / control-panel 持久化时写入 `base64:<json>`，避免 `sunshine.conf` 行解析器把 JSON 字符串里的 `]`、`#` 或换行误判为配置语法。

端口策略有两种：

1. 独立文件映射端口：实现简单、边界清楚，Moonlight 主动连出，适合第一版。
2. 复用现有 HTTPS 端口：体验更统一，但需要统一 acceptor 或从 `on_upgrade` 安全交接 socket，适合后续阶段。

推荐落地顺序：

1. `file_mapping_http` 保留 capability response/model 和后续管理接口。
2. 新增 `file_mapping_ws`，基于 Boost.Beast 实现 session 核心。
3. 第一版 capability 挂到 `nvhttp` HTTPS 端口，WSS 使用独立动态端口。
4. 后续再评估是否统一到现有 HTTPS 端口。
