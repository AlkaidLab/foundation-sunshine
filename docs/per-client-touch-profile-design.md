# 按客户端的触摸体验档案——第一层设计稿

## 1. 文档状态

- 状态：实施设计稿
- 日期：2026-09-02
- 前置：PR #1025（虚拟 USB HID 触摸屏设备 + POC，真机验证"点文本框自动弹触摸键盘"通过）
- 范围：第一层（纯 Sunshine 侧配置与注册表事务，不涉及驱动改动）
- 修订：2026-09-02 全变量隔离对照实验确认**注册表写入必需**（§5.2），事务设计恢复并扩展为全键写

## 2. 背景与目标

触摸键盘的体验由两个独立机制决定：

1. **弹不弹**：触摸 digitizer 在场 + 触摸输入 + `EnableDesktopModeAutoInvoke`
2. **长什么样**：显示器 EDID 物理尺寸档位（≥18" 强制 undocked，<18" 允许 docked）——软件手段无法覆盖，EDID 是唯一判定源（Apollo #818 实验结论）

现状：`clients` JSON 已有 per-client 的 `deviceSize`（物理尺寸 cm），经 `CREATEMONITOR {GUID}:[nits][wCm,hCm]` 传入 VDD——**EDID 档位绑定已落地**。本设计稿把"弹"的一侧（虚拟触摸屏设备）也纳入 per-client 档案，与 `deviceSize` 同级管理。对照实验（2026-09-02 全变量隔离）确认：**设备挂载提供触摸输入源，弹出策略由 TabletTip AutoInvoke 键组决定——两者都需要**（键组全 0 时不弹，恢复后弹出，见 §5.2）。

### 目标

1. `clients[]` schema 增加 `touch` 对象（启用开关、AutoInvoke 预处理）
2. 会话开始时按档案：写 AutoInvoke 注册表键组（记录 undo）；会话结束时反向还原
3. 虚拟触摸屏设备的正式化：**不属于本设计稿**——设备已从键盘路径移除，留作未来 direct touch 注入层（见 §6）
4. 任何失败不阻断流会话（触摸是增强能力，降级仅日志）

### 非目标

- 不做 EDID/物理尺寸的运行时切换（`deviceSize` 已覆盖；会话中切档会闪屏，明确不做）
- 不做第二层的 VDD EDID 参数化重构
- **不做 TabletTip 布局偏好（dockedState 等）的 per-client 覆写**——Apollo #818 实验证明此类值被 EDID 档位判定覆盖；AutoInvoke 键组与布局偏好是两类事务，前者必需（本设计稿），后者排除
- 不解决 USB 外接 digitizer 的多屏触摸路由（映射虚拟桌面，单 VDD 场景无影响）
- 不实现 `--auto` 之外的 POC UI

## 3. 现状基线（复用点）

| 组件 | 位置 | 复用方式 |
|---|---|---|
| 客户端档案容器 | `sunshine.conf` 的 `clients` 键（`config.cpp:2011-2050`），schema 校验在 `confighttp.cpp:1503-1592` | `touch` 对象直接加入现有 schema |
| 客户端识别 | `client_cert_uuid`（`pairing.cpp:493`，`launch_session_t` 携带，`rtsp.h:43`） | 会话内按 uuid 查档案 |
| 会话挂载点 | 开始：`configure_display`（`nvhttp_stream_start.cpp:222/488/688`）；结束：`restore_state`（`stream.cpp:4160-4180`） | 触摸事务挂载于 VDD 阶段之后 |
| USB/IP 设备端（direct touch 层，键盘路径无依赖） | PR #1025 `virtual_touchscreen_device` + `loopback_usbip_bridge` | 未来 input 路由接入时启用；不是键盘功能的前提 |
| 面板入口 | 控制面板 clients 管理（`/api/clients/list`） | `touch` 字段 UI 在现有客户端编辑界面扩展 |

## 4. Schema 设计

`clients[]` 每项新增可选 `touch` 对象；**缺省 = 完全维持现状**（不创建设备、不写注册表），向后兼容：

```json
{
  "uuid": "…",
  "name": "…",
  "deviceSize": "…",
  "touch": {
    "enabled": true,
    "autoInvoke": true
  }
}
```

- `enabled`：会话期间为此客户端启用触摸键盘注册表事务（前提：宿主机已有触摸 digitizer，见非目标）
- `autoInvoke`：会话期间写 AutoInvoke 注册表键组（见 §5.2，undo 到原值）。**全变量隔离对照实验（2026-09-02）证实必需**：全部 AutoInvoke 键置 0 后，设备挂载 + 触摸 tap 键盘不再弹出；键组恢复后弹出。触摸键盘自动弹出依赖这批注册表键，无法省略

校验：`touch.enabled` 为 false 时忽略其余字段。

## 5. 会话事务设计

### 5.1 时序

```text
会话开始（configure_display，VDD prepare 之后）
  ├─ 按 client_cert_uuid 解析 touch 档案；无 touch/enabled=false → 跳过
  └─ [T1] 写注册表预处理 + 记录 undo → appdata/touch_keyboard_undo.json
        失败 → 降级日志，会话继续；undo 文件已存在（并发会话）→ 跳过写入

会话结束（restore_state，最后一个视频会话注销）
  └─ [R1] 应用 touch_keyboard_undo.json 还原注册表；仅全部还原成功后删除 undo 文件（部分失败保留以供重试）
```

### 5.2 注册表项与目标 hive

| 项 | 值 | undo |
|---|---|---|
| `TabletTip\1.7\EnableDesktopModeAutoInvoke` | 1 | 记录原值（含"不存在"），结束时还原 |
| `TabletTip\1.7\EnableDesktopModeDockedAutoInvoke` | 1 | 同上 |
| `TabletTip\1.7\EnableInDesktopMode` | 1 | 同上 |
| `TabletTip\1.7\EnableInDesktopModeTabletKeyboard` | 1 | 同上 |
| `TabletTip\1.7\EnableInDesktopModeAutoInvoke` | 1 | 同上 |
| `TabletTip\1.7\EnableDockedModeAutoInvoke` | 1 | 同上 |
| `TabletTip\1.7\EnableTouchKeyboardAutoInvokeInDesktopMode` | 1 | 同上 |
| `TabletTip\1.7\EnableTouchKeyboardAutoInvoke` | 1 | 同上 |
| `TabletTip\1.7\TouchKeyboardTapInvoke` | 2 | 同上 |
| `TabletTip\1.7\PreventTouchKeyboardAutoInvoke` | 0 | 同上（仅当原值非 0 时写入） |

> **实验依据（2026-09-02，全变量隔离对照）**：上述全部键置 0 + TabTip 重启后，虚拟触摸屏 attach + tap **键盘不弹出**（IFrameworkInputPane 判定 visible=0 ×3）；键组恢复 1 后弹出。Windows 未提供任何 API/设置 UI 之外的策略接口，键组整体写入是"自动弹"的必要条件。不确定哪个键是充分键，整体写 + undo 是最稳健策略（等效于用户在设置中开启）。

目标 hive：**Sunshine 服务态（SYSTEM）写 `HKEY_USERS\<交互用户 SID>\…`**（枚举活动 console 会话令牌解析 SID）；便携态直接 HKCU。两者在事务模块内统一封装为 `write_user_registry()`。

### 5.3 已知限制（记录进文档与日志）

- TabletTip 值由 TextInputHost 进程读取并缓存，会话中写入**可能到下一次键盘进程启动才生效**；AutoInvoke 主键经验上即时生效。设计上接受"尽力而为"，不强制重启 TextInputHost（杀进程影响正在输入的用户）。
- 同机多客户端并发会话：注册表是 per-user 全局的，后写覆盖前写。第一层明确**仅支持单活动会话场景**，多会话并发时触摸事务跳过并告警（VDD 会话本身也是单活动假设）。

## 6. 虚拟触摸屏设备的定位（不在键盘路径内）

A/B 挂载实测（挂载弹 / 卸载仍弹）证明键盘体验**不依赖自建设备**——宿主机已有的触摸 digitizer（VDD / 远程软件的 `\?\VIRTUAL_DIGITIZER`）已满足输入源。PR #1025 中的 USB/IP 虚拟触摸屏设备与 POC 保留在仓库，定位重写为：

- **direct touch 输入注入的修复**：Sunshine 的合成指针触摸注入（InjectSyntheticPointerInput）在 25H2 (26200) 失效，客户端 direct touch 模式的触摸事件无法到达主机应用。USB HID 触摸屏经 hidusb 路径注入是实测可行的替代（console 会话 tap 记事本光标落入已验证）
- 依赖 usbip-win2 组件（独立部署事项）；组件缺失时仅 direct touch 注入缺失，键盘体验不受影响
- `virtual_touchscreen_session` 封装已从主 target 移除，input 路径接入时按需恢复

## 7. 代码落点

| 内容 | 文件 |
|---|---|
| schema 校验扩展 | `confighttp.cpp`（clients 路由校验处） |
| 档案读取 helper | `config.cpp` `get_client_touch_keyboard_enabled(uuid)` |
| 注册表事务模块 | `src/touch_keyboard_session.h` + `src/platform/windows/touch_keyboard_session.cpp`（hive 解析、undo json） |
| 设备 attach/detach 封装 | `src/virtual_touchscreen_session.h` + `src/platform/windows/virtual_touchscreen_session.cpp`（组合 device + bridge + usbip attach） |
| 挂载 | `nvhttp_stream_start.cpp`（开始）、`stream.cpp`（restore_state 旁结束） |
| 面板 UI | 控制面板客户端编辑界面加 touch 开关（后端字段透传即可） |

## 8. 测试计划

- 单测：`touch` schema 校验、undo 写入/还原、无 touch 字段的向后兼容
- 键组依赖消融（已完成 2026-09-02）：全键置 0 + TabTip 重启 → 不弹；恢复 → 弹
- A/B 挂载实测（已完成 2026-09-02）：挂载弹 / 卸载仍弹——键盘体验不依赖自建设备，架构据此简化
- 真机（console 会话）：点文本框 → 键盘弹出；会话结束后注册表键组还原、再次会话重新生效
- 降级：任何注册表事务失败时流会话不受影响
- 断电类异常：undo 文件残留时下次会话开始先尝试还原

## 9. 发布闸门

1. `clients` schema 变更需控制面板同步发版（新字段 UI）
2. usbip-win2 组件版本固定（当前 0.9.7.7）仅约束未来 direct touch 注入层；键盘注册表事务无此依赖
3. 注册表写入面限定于 §5.2 键组（含 undo），键集合变更需评审
