# 游戏设置与启动参数研究

检索日期：2026-11-11。本文只记录社区项目公开文档、许可证和可重复观察到的文件行为；没有复制第三方代码。GPL/AGPL 项目只作为行为资料，MIT/BSD/Apache 项目也没有把代码直接移入本仓库。

## 安全边界

Host 只在游戏进程未运行时处理游戏自己的配置文件。文件型设置先复制到 `game-settings-backup/<install>.ini.before`，写到同目录临时文件后原子替换，再重新解析关键值；应用结束时，如果文件哈希仍等于本次写入后的哈希，`restore` 才恢复备份，否则保留游戏或用户后来写入的内容。找不到文件、没有 `GameUserSettings` 段、键值格式不明或回读不一致时，项目返回 `unavailable`/`write_failed`，不猜测、不写入。

启动参数只来自 catalog 描述的枚举、布尔和整数项。Host 会生成受控的 `-screen-width`、`-screen-height`、`-screen-fullscreen`、`-popupwindow`、Unity 的 `-force-d3d11/-force-d3d12` 或 Unreal 的 `-dx11/-dx12`；没有自由文本命令行输入。Steam/Epic 链接和需要签名父进程的 Hypergryph 游戏不追加参数。

## 各标题

| 标题 | 社区项目与许可证 | 项目读取/写入的资料 | Host 当前设置与启动项 | 120 FPS |
| --- | --- | --- | --- | --- |
| 原神 | Collapse Launcher，MIT；Starward，MIT（已有 host 研究记录） | 启动器注册表、Unity PlayerPrefs/注册表设置；社区启动器读写显示、画质、VSync、渲染比例、HDR | 延续现有注册表适配；catalog 保留 30/45/60；启动项为分辨率、全屏、popup、DX 选择 | 游戏自身档位最高 60；Sparxie 的独立解锁项仍是另一路，未把内存实现移入本变更 |
| 崩坏：星穹铁道 | Collapse Launcher，MIT；Starward，MIT | 注册表中的 GraphicsSettings 模型与分辨率；启动器提供 30/60/120、VSync、画质和渲染比例 | 延续现有读写；catalog 保留 30/60/120；启动项同 Unity 项 | 游戏自身支持 120，已由现有 fixture 覆盖 |
| 绝区零 | Collapse Launcher，MIT；Starward，MIT | Unity `GENERAL_DATA.bin` 与注册表屏幕值；文件需要版本 magic | 延续现有受 magic 保护的读写；catalog 的文件项仍在 magic 可用时显示；启动项同 Unity 项 | 游戏自身档位为 30/60/不限；社区解锁属于另一路，未移植 |
| 崩坏3 | Collapse Launcher，MIT；`Hi3Helper.Plugin.Hypergryph`，MIT（当前上游要求迁移到 Hypergryph 插件） | 注册表 JSON blob 的屏幕和个人画质数据 | Host 读写分辨率、窗口模式、30/60；其余无可靠域则 unavailable；启动项同 Unity 项 | 未发现可由游戏自身设置证明的 120；未验证实机 |
| 鸣潮 | `mysteriousmy/WuwaTouchLauncher` v0.7，MIT；`AFNGP/ww_120_fps_launcher` 2.2，MIT；`WuwaTouchLauncher` 文档记录 56 项菜单键和 Unreal 配置 | `Client/Saved/Config/WindowsNoEditor/GameUserSettings.ini`，包括分辨率、FullscreenMode、FrameRateLimit、VSync、ResolutionQuality、OverallQuality；120 解锁工具刻意避免把 `LocalStorage.db` 设为只读 | Host 当前安全读写上述六个稳定核心键，并暴露完整启动描述；未知扩展键保持 unavailable，后续可按同一类字段扩展；启动项支持分辨率、窗口、popup、DX11/DX12 | 游戏菜单/配置档记录 30/45/60/90/120/144/165/240/不限；120 可达性来自社区文档，未在本机实机验证 |
| 战双帕弥什 | 未找到许可证明确且同时公开 PC 图形设置格式的可靠社区项目；官方/Steam 启动由现有检测支持 | 仅找到启动器/可执行文件资料，没有稳定设置文件格式 | catalog 只显示受控启动项；设置保持自动/不可用，不写未知文件 | 未验证；没有把内存解锁当作设置支持 |
| 明日方舟 | `Hi3Helper.Plugin.Arknights`，MIT（已迁移到 Hypergryph 插件）；`HuaXiaoLing-Launcher` v2.10.2，MIT | 主要读取官方/Yostar 启动器记录、渠道和资源；未公开稳定图形设置 writer | catalog 显示启动项和 `launcherParent=official`；direct_command 优先官方 Hypergryph Launcher，避免 10100 父进程失败；没有未知格式写入 | 未验证；未声称支持 120 |
| 明日方舟：终末地 | `Kelcoin/EndfieldDirectLauncher`，MIT；`AugustLigh/LLauncher`，MIT；`HuaXiaoLing-Launcher`，MIT | 社区项目读取安装目录、启动器和签名父进程；EndfieldDirectLauncher 文档说明 `Games.exe` 名称与发行商签名检查，LLauncher 记录 DX/窗口等启动选项 | catalog 显示分辨率、窗口、启动参数和 `launcherParent=official`；Host 只使用官方启动器路径，不复制或伪造签名父进程；配置格式未知时不写 | **没有 120**；catalog 仅提供 30/60，实机帧率仍未验证 |
| 异环 | `NTE-Maid`（仓库未声明 SPDX，本文只作行为资料）；`HuaXiaoLing-Launcher`，MIT | NTE-Maid 说明无侵入，不修改游戏数据；社区资料指向 `Saved`/`Saved_Global` Unreal 配置目录，但没有稳定 writer 许可格式 | Host 对已存在且含 `GameUserSettings` 段的文件提供六个核心键读写；未找到文件时全部 unavailable；启动项支持窗口、分辨率、popup、DX11/DX12 | 未验证；catalog 仅列出 30/45/60/90/120/144/165/240 作为文件可接受档，实机接受范围未证实 |

## 启动器方案比较

Collapse/Starward 的优势是对 HoYoverse 的渠道、注册表和 JSON 模型有长期维护；Host 延续这些稳定格式并保留版本不可读时的 unavailable。鸣潮的最新公开方案把“配置文件预置”和“运行时内存实验”分开，且明确用原子保存和备份恢复；本变更采用前者，避免把内存注入误报为设置支持。Hypergryph 游戏的领先方案都把官方启动器/签名父进程视为启动约束；Host 选择可验证的官方 Launcher 路径，没有采用伪造父进程的实现。NTE-Maid 的无侵入策略是未知格式保持不动的直接依据。

## 实现与验证边界

已实现：catalog 为九个现有标题加入与其资料相符的启动设置；终末地明确没有 120；鸣潮和异环增加 Unreal 配置文件的安全读写、备份、原子替换、回读和退出恢复；现有 HoYoverse/ZZZ 适配保持原路径；direct launch 将受控参数拼入可执行文件命令，并跳过商店链接和 Hypergryph 官方父进程路径。

未实现：鸣潮社区工具的运行时内存 FPS/画质注入、任意命令行、父进程伪造、PGR/Arknights/Endfield/NTE 的未证实私有格式，以及真实游戏内 120/画质效果验证。UI 复用现有 catalog 字段渲染；对新 launch 分组的视觉润色和专门文案属于后续 UI 工作。

性能与系统状态：配置读写只在启动/保存边界发生，热路径没有新增循环、网络 syscall 或持续分配；未测量字节吞吐、P95/P99、磁盘延迟或与社区实现的同机基准。Windows 源代码路径已补齐，Linux 单元测试可验证格式逻辑；Windows VM 构建/测试、真实 Windows 游戏、资源授权、跨租户隔离均未验证。外部协议兼容性：本能力可经 Adapter 映射到 Steam/Epic 启动链接、Unity 命令行参数、Unreal GameUserSettings 配置和各官方启动器的安装记录；这些协议适配仍属于 host 的现有 launcher boundary，不改变平台核心接口。

## 资料

- Collapse Launcher：<https://github.com/CollapseLauncher/Collapse>，MIT，README 和公开设置/插件资料，2026-11-11 读取。
- Starward：<https://github.com/starward-project/starward>，MIT，公开 launcher/settings 资料，2026-11-11 读取。
- Wuthering Waves 120 FPS Launcher：<https://github.com/AFNGP/ww_120_fps_launcher>，MIT，README v2.2，2026-11-11 读取。
- WuwaTouchLauncher：<https://github.com/mysteriousmy/WuwaTouchLauncher>，MIT，v0.7 `docs/GRAPHICS-OPTIONS.md` 和 `docs/MEMORY-GRAPHICS.md`，2026-11-11 读取。
- EndfieldDirectLauncher：<https://github.com/Kelcoin/EndfieldDirectLauncher>，MIT，README，2026-11-11 读取。
- NTE-Maid：<https://github.com/leck995/NTE-Maid>，仓库未声明 SPDX，README，2026-11-11 读取。
- HuaXiaoLing-Launcher：<https://github.com/Sparkle0v0Hxl/HuaXiaoLing-Launcher>，MIT，v2.10.2 README，2026-11-11 读取。
- Hi3Helper Hypergryph plugin：<https://github.com/misaka10843/Hi3Helper.Plugin.Arknights>，MIT，README 的迁移说明，2026-11-11 读取。
- Microsoft Learn `MoveFileExW`：<https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw>，当前页面，`MOVEFILE_REPLACE_EXISTING`/`MOVEFILE_WRITE_THROUGH`，2026-11-11 读取；沿用仓库 source-index 记录 `ds5-ms-movefileex-20261107`。
