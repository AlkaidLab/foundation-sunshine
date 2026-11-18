# Windows 兼容性说明

## 支持范围

**底线是 Windows 10 1507（build 10240），不是 22H2。** 车主要求兼容很老的 Windows 10：凡是技术上能跑主机的 Windows 10 都可以安装，只有低于 Windows 10 时安装器才拒绝（Inno `MinVersion=10.0`；安装壳在 Windows 10 以前给出一句提示后退出，退出码 1150，不会崩溃）。某个组件需要更新的系统时，只是不提供这个组件：安装页里它不勾选、不能勾选，并写明原因，例如「需要 Windows 10 1903 或更高」；静默安装通过 Inno 组件的 `MinVersion` 同样跳过它。

| 部分 | 最低 Windows 10 | 低于时 | 依据 |
| --- | --- | --- | --- |
| 主机程序 `sunshine.exe`、服务、安装壳（MinGW 静态链接，UCRT 随 Windows 10 提供；较新的 API 运行时查找） | 1507（10240） | — | 安装壳导入表见下文 |
| 屏幕采集 | 1507（10240） | 1903 之前没有 WGC 时用 DXGI Desktop Duplication（Windows 8 起） | `installer-ms-desktop-duplication-20261109`、`ds5-ms-wgc-20261109` |
| 管理界面（Panel，WebView2） | 1709（16299）；LTSC 2015/2016/2019/2021 也受支持 | Panel 打不开；主机照常装好，用网页管理 `https://localhost:47990` | `installer-ms-webview2-platforms-20261109` |
| 基地虚拟显示器（VDD，两个驱动包都是 UMDF 2.25 + IddCx 1.2） | 1803（17134） | 组件不提供 | `installer-ms-umdf-version-history-20261109`、`installer-ms-iddcx-versions-20261109` |
| 虚拟手柄（ViGEmBus 1.22） | Windows 10 任一版本（1.17 起只支持 Windows 10/11） | — | `installer-vigembus-readme-20261109` |
| USB/IP（usbip-win2） | 1903（18362），x64 | 组件不提供 | `installer-usbip-win2-readme-20261109` |
| 虚拟鼠标（UMDF 2.15） | 1507（10240） | — | `installer-ms-umdf-version-history-20261109` |
| HDR（虚拟显示器的 HDR 能力） | 随显示驱动和系统 HDR 支持而定；不作为安装门槛 | 不显示 HDR 选项 | — |

此前这里写的「最低 Windows 10 22H2（19045），安装器应拒绝更低版本」不对：它不是任何组件的真实要求，已更正（2026-11-09）。

Windows Graphics Capture（WGC）在 Windows 10 1903（build 18362）开始提供；更早的系统和服务会话使用 Desktop Duplication。

## Windows 10 与 Windows 11 的差异

| 功能 | Windows 10 | Windows 11 |
| --- | --- | --- |
| 管理界面 | 使用 WebView2 Runtime；缺少 Runtime 时显示原生 Win32 对话框，可下载 Runtime 或打开 `https://localhost:47990` | Windows 11 通常随系统提供 WebView2，但启动前仍检查；缺少时使用相同的对话框 |
| 窗口圆角 | `DWMWA_WINDOW_CORNER_PREFERENCE` 不可用时保留方角和纯色背景 | DWM 可提供圆角；失败时仍保留可用窗口 |
| 背景材质 | 使用不依赖材质 API 的纯色背景 | 可使用系统合成效果；界面功能不依赖 Mica 或圆角 |
| 字体 | `Segoe UI`、`Microsoft YaHei UI` 回退；不依赖 Segoe UI Variable | 优先使用 Variable 字体，缺失时使用相同回退栈 |
| 图标 | 使用随系统提供的稳定字体回退 | 可使用较新的系统图标资源；缺失时保留文字和稳定回退 |
| 屏幕捕获 | WGC 可用时使用 WGC；服务会话使用 Desktop Duplication | 使用同一路径，并按运行时能力选择 |

## WebView2 Runtime 处理

Panel 在创建任何 WebView2 窗口前检查 Evergreen Runtime。检查顺序使用 Microsoft Learn 规定的 EdgeUpdate `pv` 值，覆盖每机安装和每用户安装，并要求版本值存在、可解析且大于 `0.0.0.0`。Panel 也支持 `SUNSHINE_GUI_FORCE_NO_WEBVIEW2` 测试覆盖，用于在不卸载 Runtime 的情况下验证缺失路径。

缺少 Runtime 时，Panel 显示系统语言对应的原生 Task Dialog。下载按钮优先运行 Panel 目录旁已随包提供的 bootstrapper，没有 bootstrapper 时打开 Microsoft 的 bootstrapper 链接；网页管理按钮打开 `https://localhost:47990`；关闭按钮结束进程并返回专用退出码。宿主服务识别该退出码，连续三次短运行后停止自动重启并记录 `WebView2 Runtime missing; GUI restart suppressed after 3 failures`，同时保留低频重新附加已由用户启动的 GUI 的路径。

## API 与降级规则

宿主的 Windows API 审计以 Windows 10 1507（10240）为最低运行目标。使用 Windows 11 专有的视觉属性时必须允许 API 调用失败，并继续使用纯色背景、方角和稳定字体。需要更高系统版本的 API 不得作为 Panel 或宿主的静态启动依赖；能力检测必须在运行时进行。

本卡对以下发布目录的 PE 导入表需要在 Windows 构建机复核：

| 文件 | 目录 | 审计状态 |
| --- | --- | --- |
| `sunshine-gui.exe` | 安装包暂存目录 `build\inno_staging` 与发布目录 | 未核实：本次工作树没有这些 Windows 构建产物 |
| `sunshine.exe` | 同上 | 未核实：本次工作树没有这些 Windows 构建产物 |
| `sunshinesvc.exe` | 同上 | 未核实：本次工作树没有这些 Windows 构建产物 |
| 工具 | 同上 | 未核实：本次工作树没有这些 Windows 构建产物 |

Windows 构建和导入表命令由控制器在 Windows test host 上执行；本地没有伪造审计结果。

## 依据

- `ds5-ms-webview2-distribution-20261109`：Microsoft Learn《Distribute your app and the WebView2 Runtime》，检测 `pv` 注册表值、Runtime API、bootstrapper 和每机/每用户安装。
- `ds5-ms-webview2-idl-20261109`：Microsoft Learn WebView2 Win32 IDL，`GetAvailableCoreWebView2BrowserVersionString` 的返回和错误语义。
- `ds5-ms-taskdialogindirect-20261109`：Microsoft Learn `TaskDialogIndirect`，原生 Task Dialog 的按钮和配置结构。
- `ds5-ms-dwmwindowattribute-20261109`：Microsoft Learn `DWMWINDOWATTRIBUTE`，窗口圆角和不可用属性的运行时失败行为。
- `ds5-ms-wgc-20261109`：Microsoft Learn Windows.Graphics.Capture，Windows 10 1903 起的捕获能力。
- `ds5-ms-win10-support-20261109`：Microsoft Learn Windows 10 Home and Pro 生命周期（服务支持期，不是运行门槛）。
- `installer-ms-webview2-platforms-20261109`：Microsoft Learn WebView2 概览，支持的 Windows 10 版本（SAC 1709 起，LTSC 2015/2016/2019/2021）。
- `installer-ms-umdf-version-history-20261109`：Microsoft Learn UMDF 版本历史（2.15 = 1507，2.25 = 1803）。
- `installer-ms-iddcx-versions-20261109`：Microsoft Learn IddCx 版本。
- `installer-ms-desktop-duplication-20261109`：Microsoft Learn Desktop Duplication API（Windows 8 起）。
- `installer-usbip-win2-readme-20261109`：usbip-win2 README，Requirements：Windows 10 x64 1903（18362）或更高。
- `installer-vigembus-readme-20261109`：ViGEmBus README，1.17 起只支持 Windows 10/11。

这些来源只用于 API 和版本事实；没有复制第三方实现代码。
