# Windows 目录映射普通用户交互方案

本文补充 `windows_directory_mapping_design.md`：实现必须让普通用户能理解和接受，而不是要求用户手写 JSON、理解 WSS token 或区分 host/client UUID。

## 交互原则

- 默认只读，用户主动开启写入、删除、覆盖等高风险能力。
- 以“设备 + 文件夹 + 权限”表达，不暴露协议细节。
- 低风险动作可以一键完成，但必须可撤销、可追踪。
- 所有风险动作使用普通语言解释后果。
- 失败提示给出下一步，而不是只显示错误码。

## 产品分层

目录映射的用户入口由 Rust GUI / Control Panel 承担，安全边界和文件数据面由 Sunshine core 承担。

```text
Explorer 右键菜单
  -> Rust GUI / Control Panel helper
      -> 本机路径校验
      -> 快速创建安全默认 mapping
      -> 通知和管理入口
      -> 调 Sunshine 本地管理 API
          -> Sunshine core
              -> 配置持久化
              -> 路径安全裁决
              -> 设备授权裁决
              -> nvhttp capability
              -> Beast WSS 文件 RPC
```

Rust GUI / Control Panel 负责：

- 右键菜单注册和移除。
- 接收 `--quick-share-folder <path>`。
- 展示共享列表和高级设置。
- 显示系统通知、撤销入口、最近访问和审计摘要。
- 调用 Sunshine 本地管理 API 创建、修改、删除 mapping。

Sunshine core 负责：

- 最终路径校验和 reparse point 拦截。
- paired client certificate / token / UUID 校验。
- mapping 授权裁决。
- 文件 I/O 和传输协议。
- 审计事件落盘。

Rust GUI 不能成为安全边界；即使 UI 或右键入口传入了错误配置，core 也必须拒绝不安全访问。

## Sunshine 侧体验

Sunshine Web UI 增加“文件夹共享”页面。

### 资源管理器右键入口

Windows 主机端安装 Sunshine 后，可以在目录右键菜单中注册入口：

```text
通过 Sunshine 共享
```

用户路径：

1. 用户在资源管理器中右键一个文件夹。
2. 点击“通过 Sunshine 共享”。
3. Rust GUI / Control Panel helper 校验该路径是本机目录。
4. 使用安全默认值立即创建共享。
5. 系统通知提示“已通过 Sunshine 共享此文件夹”。
6. 用户需要调整权限时，再进入 Control Panel 的文件夹共享页。

右键入口可以直接创建共享，但只能使用安全默认权限，不允许静默开启写入、删除、执行或 reparse point 穿透。

推荐命令模型：

```text
sunshine-control-panel.exe --quick-share-folder "%V"
```

`--quick-share-folder` 的行为：

- 校验参数必须是本机存在的目录。
- 生成稳定 mapping id 和显示名称。
- 调 Sunshine 本地管理 API 创建 mapping。
- 默认只读。
- 默认允许所有已配对设备访问，后续可在 Control Panel 改成指定设备。
- 默认拒绝 reparse point / symlink / junction。
- 默认不允许删除、不允许执行。
- 如果 Sunshine 服务未运行，提示用户启动 Sunshine 后重试。
- 如果当前没有已配对设备，仍可创建共享，但通知提示“还没有已配对设备可以访问”。

快速共享默认值：

```text
mode = read
allow_delete = false
allow_execute = false
follow_reparse_points = false
max_file_size = 0
clients = []
```

其中 `clients = []` 表示所有已配对设备可访问。它不表示匿名开放；访问仍然必须通过 paired client certificate、capability token 和 WSS hello UUID 校验。

成功反馈：

```text
已通过 Sunshine 共享“Downloads”
只读访问 · 仅已配对设备
```

通知动作：

```text
打开共享设置
撤销
```

如果系统通知不支持动作，至少打开 Control Panel 的文件夹共享页，并高亮新建项。

注册位置建议使用当前用户范围，避免不必要的管理员权限：

```text
HKCU\Software\Classes\Directory\shell\Sunshine.ShareFolder
HKCU\Software\Classes\Directory\Background\shell\Sunshine.ShareFolder
```

其中：

- `Directory\shell` 覆盖“右键某个文件夹”。
- `Directory\Background\shell` 覆盖“在文件夹空白处右键共享当前目录”。

菜单文案：

```text
MUIVerb = 通过 Sunshine 共享
Icon = <sunshine.exe>,0
```

命令：

```text
"<install_dir>\sunshine-control-panel.exe" --quick-share-folder "%V"
```

便携版不应默认写注册表；可以在设置中提供：

```text
添加资源管理器右键入口
移除资源管理器右键入口
```

卸载时必须删除上述注册表项。

如果未来使用 Windows 11 modern context menu，可以再补 COM `IExplorerCommand` shell extension；第一版先用 classic shell verb 更稳，代码少、容易卸载，也不会引入 Explorer 进程内崩溃风险。

空状态：

```text
还没有共享文件夹
添加一个文件夹后，已配对的 Moonlight 设备可以在串流时访问它。
```

添加共享向导：

1. 选择本机文件夹。
2. 设置显示名称，默认使用文件夹名。
3. 选择允许的设备，默认“仅当前已配对设备”。
4. 选择权限，默认“只读”。
5. 确认风险摘要。

权限文案：

```text
只读：Moonlight 可以查看和下载文件，不能修改。
```

读写、删除、执行和符号链接/junction 穿透属于后续高级能力。第一阶段 UI 不展示可开启入口，core 也会拒绝或降级这些字段。

高级选项默认折叠：

```text
最大文件大小
```

默认值：

```text
mode = read
allow_delete = false
allow_execute = false
follow_reparse_points = false
max_file_size = 0
clients = 当前选择的设备 UUID 列表
```

共享列表卡片展示：

```text
名称
本机路径
允许设备数量
权限：只读
状态：可用 / 路径不存在 / 被安全策略阻止
最近访问时间
```

危险状态提示：

```text
此共享包含符号链接或 junction，已被默认安全策略阻止。
```

## Moonlight 侧体验

串流内增加“文件”入口，而不是把能力藏在调试环境变量里。

首次进入：

```text
这台主机共享了 2 个文件夹
你可以查看或下载主机允许的文件。Moonlight 不会自动访问文件，除非你打开此页面。
```

如果 Sunshine 要访问 Moonlight 本地目录，Moonlight 必须弹出首次授权：

```text
Sunshine 想访问你的文件夹
主机：Living Room PC
请求访问：Downloads
权限：只读

允许一次 / 始终允许这台主机 / 拒绝
```

文件页面基础交互：

```text
主机文件夹
本机文件夹
下载
取消
重试
在文件夹中显示
```

### Explorer 挂载体验

桌面端 moonlight-qt 技术上可以把 Sunshine 共享目录挂载到 Windows Explorer，但这不应作为第一版默认入口。

第一版优先做内置文件面板，因为它不需要安装文件系统驱动，不会让其他本机程序静默访问远端文件，也更容易把错误解释清楚。Explorer 挂载应作为后续可选增强：

```text
打开文件面板
下载到本机
在文件夹中显示

可选：
挂载到资源管理器
取消挂载
```

如果用户选择挂载，必须先展示一次明确提示：

```text
将远端文件夹挂载到资源管理器？

挂载后，本机上的其他程序也可以像读取普通磁盘一样读取这些远端文件。
当前仅支持只读访问。断开串流、Moonlight 退出或网络中断后，挂载会自动移除。
```

挂载入口规则：

- 默认不自动挂载。
- 只在桌面 Windows 版显示。
- 需要 WinFsp/Dokany 等文件系统组件可用；未安装时只显示“文件面板”和“下载到本机”。
- 第一阶段只能挂载为只读。
- 挂载仅在当前主机连接有效期内存在，断线或退出 Moonlight 必须自动卸载。
- 同一台主机的挂载名使用可读名称，例如 `Sunshine - Living Room PC`。
- 不承诺固定盘符。优先让系统分配或让高级用户选择。
- 错误提示必须落到普通语言，例如“文件系统组件未安装”“主机已断开”“此共享已被主机撤销”。

挂载不是安全边界。真正的权限、路径解析、reparse point 拒绝和文件大小限制仍由拥有目录的一端执行。

传输任务必须显示：

```text
文件名
来源和目标
进度
速度
剩余时间
取消按钮
失败原因
```

## 错误提示映射

| 错误码 | 普通用户文案 |
| --- | --- |
| `forbidden` | 这台设备没有访问此文件夹的权限。 |
| `absolute_path` | 路径无效。只能访问共享文件夹内的相对路径。 |
| `invalid_path` | 路径包含不支持的字符或跳出共享范围。 |
| `path_escape` | 已阻止访问共享文件夹外的路径。 |
| `reparse_point_blocked` | 已阻止符号链接或 junction，避免访问到未共享位置。 |
| `file_too_large` | 文件超过此共享允许的大小。 |
| `not_found` | 文件不存在，可能已被移动或删除。 |
| `upgrade_required` | 文件通道未建立，请重新连接串流。 |

## PR 交付建议

当前 PR 可以先交付协议、服务端能力、本机管理 API、Explorer 右键快速共享和 Control Panel 最小管理闭环，但必须在描述中明确：

- Web UI 管理页尚未接入。
- Moonlight 文件面板尚未接入。
- Moonlight Explorer 挂载尚未接入，且不作为第一版默认 UX。
- 高级权限设置仍以 core API 能力为主，Control Panel 当前只暴露普通用户最容易理解的快速共享、列表和移除。
- 默认安全策略已经按未来 UI 的默认值实现：只读、拒绝 reparse point、按设备授权，且只读模式不允许删除。

后续 UI PR 应先做“只读下载”闭环，再考虑可选 Explorer 挂载，最后再开放上传、删除和双向共享。

## 对当前实现的修改点

按上述分层审视，当前实现还需要做这些调整：

### 1. 从静态配置扩展为可管理 mapping store

当前 Sunshine core 只在启动时解析 `config::nvhttp.file_mappings`。右键快速共享需要运行时创建、删除和更新 mapping，因此需要新增一个 core 级管理模块：

```text
file_mapping_store
  load()
  save()
  list()
  add_quick_share(path)
  update(id, patch)
  remove(id)
```

`file_mapping_store` 仍然输出 `std::vector<mapping_t>` 给 WSS operations context，但配置持久化和默认值归一化不应散落在 Rust GUI 里。

管理 API 写入必须按事务语义处理：先保留 store 快照，内存变更通过校验后再写入 Sunshine 配置；如果配置持久化失败，需要恢复旧 store，且不能提前更新 `config::nvhttp.file_mappings`。权限归一化也留在 core：第一阶段固定只读，`readwrite`、删除、执行和 reparse point 穿透都不能由 Rust GUI 或本机 API 打开。

### 2. 新增本地管理 API

Control Panel / Rust GUI 不应直接编辑 Sunshine 配置文件。应调用本机管理 API：

```text
POST /api/v1/file-mapping/mappings
GET /api/v1/file-mapping/mappings
PATCH /api/v1/file-mapping/mappings/{id}
DELETE /api/v1/file-mapping/mappings/{id}
```

其中快速共享请求可以是：

```json
{
  "path": "D:/Downloads",
  "source": "explorer_context_menu"
}
```

core 返回：

```json
{
  "ok": true,
  "mapping": {
    "id": "downloads-a1b2c3",
    "name": "Downloads",
    "path": "D:/Downloads",
    "mode": "read",
    "clients": []
  }
}
```

### 3. WSS operations context 需要支持热更新

当前 `file_mapping::service_t` 创建 WSS server 时把 mappings 注入 `execution_context_t`。运行时新增共享后，WSS 需要看到最新 mappings。

建议把：

```text
execution_context_t.mappings
```

改为由 thread-safe provider 提供：

```text
mapping_provider_t::snapshot()
```

这样 Rust GUI 新增 mapping 后，无需重启 Sunshine，也无需重建 WSS listener。

### 4. Rust GUI 负责系统集成，不负责安全裁决

Rust GUI 需要新增：

```text
--quick-share-folder <path>
--open-file-sharing
--install-explorer-menu
--uninstall-explorer-menu
```

当前已落地的 Control Panel 最小闭环：

- `--quick-share-folder <path>` 会校验本机目录并调用 Sunshine 本机管理 API 创建安全默认共享。
- 设置页增加“文件夹共享”卡片，可添加文件夹、刷新列表、撤销共享。
- 设置页提供右键菜单注册和移除入口。
- 右键菜单注册到当前用户 `Directory\shell` 和 `Directory\Background\shell`，不需要管理员权限。
- Tauri command 已暴露 `quick_share_folder`、`list_file_mappings`、`delete_file_mapping`、`update_file_mapping`、`install_file_mapping_menu`、`uninstall_file_mapping_menu`。

后续仍需要补：

- `--open-file-sharing` 用于从通知或右键完成后直接打开并高亮共享项。
- 系统通知的成功/失败反馈和撤销动作。
- 高级权限编辑 UI，包括指定设备、读写、删除、执行和 reparse point 策略。

但 Rust GUI 只做体验层校验：

- 路径是否存在。
- 是否是目录。
- 失败时显示清晰提示。
- 成功时发系统通知。

最终安全判断仍由 Sunshine core 执行，包括 reparse point、路径逃逸、设备授权和文件大小限制。

### 5. Installer / portable 行为

安装版可以默认注册当前用户右键菜单。便携版不要默认写注册表，应在 Control Panel 里提供显式开关。

卸载时必须删除：

```text
HKCU\Software\Classes\Directory\shell\Sunshine.ShareFolder
HKCU\Software\Classes\Directory\Background\shell\Sunshine.ShareFolder
```

### 6. 审计事件要从 core 产生

右键快速共享、撤销、权限修改、设备访问、访问被拒绝都应进入审计日志。Rust GUI 可以展示审计，但不能伪造审计事实。

建议事件：

```text
mapping_created_quick_share
mapping_removed
mapping_updated
mapping_access_allowed
mapping_access_denied
reparse_point_blocked
path_escape_blocked
file_too_large_blocked
```
