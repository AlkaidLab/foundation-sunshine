# 2026-11-12 更新：直接启动改用 Collapse 插件的方式

所有者明确要求并接受账号风险：终末地的游戏启动方式使用 Collapse 终末地插件的启动方式。本节覆盖下文“父进程伪造路径，本仓库不采用”“保留直接 `Endfield.exe` 的回退”等旧结论；旧文保留作研究记录。读取的源码是插件仓库 misaka10843/Hi3Helper.Plugin.Endfield 的提交 `1efd304855ed68535d50e11504d790cd7004a3b1`（MIT，Copyright (c) 2025 Collapse Launcher，来源键 `endfield-collapse-*-1efd304`）。

## 插件的精确方法

- `Hi3Helper.Plugin.Endfield/Exports.GameLaunch.cs:38-41`：以游戏 EXE 路径和用户配置的 `ParentExecutablePath` 调用 `GameDecoyParentBridge.Create`，再 `parent.StartGame(游戏EXE, 启动参数, 游戏所在目录)`；`:44-45` 用返回的 `Process` 设置优先级；`:52` 以 `process.WaitForExitAsync` 观察游戏退出；`using var parent` 在游戏退出后释放诱饵父进程。
- `Hi3Helper.Hypergryph.Core/Management/GameDecoyParentBridge.cs:21`：诱饵来源默认是游戏目录下的 `PlatformProcess.exe`（注释写明：Endfield.exe 要求提升，PlatformProcess.exe 可由普通启动器挂起），相对路径按游戏目录解析；`:22-23` 必须是存在的 `.exe`；`:24` 来源与游戏 EXE 做签名者校验；`:26-32` 在 `<游戏目录>\_Hi3_Hg\parent\<本进程pid>\<guid>\` 下复制为 `Games.exe` 并清除只读属性；`:33` 对复制件再次校验；`:34` 以挂起状态启动；`:45-52` 释放时删除该目录。
- `Hi3Helper.Hypergryph.Core/Management/ParentProcessBridge.cs`：`:25` 客户端接受的父进程文件名固定为 `Games.exe`；`:147-161` 以 `CREATE_SUSPENDED | CREATE_NO_WINDOW` 创建诱饵，工作目录为其所在目录，**永不恢复**；`:48-109` `StartGame`：`InitializeProcThreadAttributeList`（1 项）→ `UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PARENT_PROCESS=0x20000, 诱饵进程句柄)`；命令行仅在有参数时为 `"<exe>" <参数>`；`CreateProcessW(应用名=游戏EXE, ..., bInheritHandles=false, 创建标志=EXTENDED_STARTUPINFO_PRESENT, 环境=nullptr, 工作目录=游戏目录, STARTUPINFOEX)`；之后关闭线程与进程句柄，用 PID 取回 `Process`；`:111-138` `Dispose`：`TerminateProcess` 诱饵、`WaitForSingleObject(2000)`，再由子类删除暂存目录；`:167-202` Authenticode 校验：对游戏和来源各做 `WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2, WTD_UI_NONE, 无吊销检查, WTD_CACHE_ONLY_URL_RETRIEVAL)` 必须为 0，且两者签名者证书（`X509Certificate.CreateFromSignedFile`）的 SHA-256 必须相同，没有硬编码的发行者白名单。该校验的作用是保证被复制成 `Games.exe` 的只能是与游戏同一签名者的映像。
- 插件没有给 Endfield.exe 传额外环境变量，命令行参数由用户的启动参数字段给出。

## 本仓库的实现

- `tools/sparxie-launch/DecoyParent.cs`：上述方法的 C# 移植（保留署名，见 `NOTICE`），并增加两处：创建后用 `NtQueryInformationProcess` 确认游戏的父进程确实是诱饵（否则终止游戏并报错），启动前清理已退出启动器遗留的暂存目录。
- `tools/sparxie-launch/DirectLaunch.cs`：Endfield 的直接启动先走诱饵父进程；签名或来源不满足时记录一行警告后退回普通直接启动（最后的保底，可能得到 10100）。诱饵与游戏同属启动器的 job（kill-on-close），游戏退出后诱饵被终止、暂存目录被删除。
- `tools/sparxie-launch --endfield-direct <Endfield.exe> [--parent-source <映像>]`：宿主非 Sparxie 的终末地应用使用的模式；`src/sparxie/host.cpp` 的 `direct_command` 对非商店的 Endfield 安装生成该命令（随宿主发布的启动器存在时），官方启动器 `Launcher.exe` 不再用于 Endfield；Arknights 与 Steam/Epic 的启动方式不变。
- Sparxie 一侧见其仓库分支 `feat/endfield-collapse-launch`：`native/EndfieldCore/endfield_decoy.cpp`。

引用的官方文档：UpdateProcThreadAttribute（`PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`：lpValue 为父进程句柄指针，句柄需要 `PROCESS_CREATE_PROCESS`）、Process Creation Flags（`EXTENDED_STARTUPINFO_PRESENT`、`CREATE_SUSPENDED`）、CreateProcessW（`STARTUPINFOEX`）、WinVerifyTrust，来源键见 `docs/authority/source-index.tsv` 的 `ms-*-20261112`。

## 未核实

真实 Endfield 客户端是否接受该父进程、反作弊/账号后果、真实游戏目录中是否存在 `PlatformProcess.exe` 均未在本机核实；测试使用假游戏。

# 终末地启动与触控研究（2026-11-11）

本文只记录本卡实际阅读到的资料和本仓库实现选择。社区项目的源代码只用于观察行为；没有复制 GPL/AGPL 代码。来源键见 `docs/authority/source-index.tsv`。

## 结论

- PC 客户端进程名是 `Endfield.exe`。官方启动器安装目录可由 Windows 卸载项的 `InstallLocation` 找到；启动器的版本目录或根目录下再找 `Launcher.exe`。宿主现在同时查找 `Program Files`、`Program Files (x86)`、`%LOCALAPPDATA%` 的常见目录，以及 CN、Global、B 服三条卸载项记录。自定义目录和按用户安装因此也能进入同一条启动路径。
- CN、B 服、Global 的非商店安装都保存为官方启动器命令；Steam 用 `steam://rungameid/4732690`，Epic 使用安装清单中记录的 `AppName` 组成官方 URI。应用仍标记为 elevated，因为现有宿主模型已验证这些游戏清单会要求管理员权限并可能返回 Windows 错误 740。
- Collapse 的 Endfield 插件采用 `Games.exe` 作为挂起的直接父进程，并通过 `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` 创建游戏；其代码还做 Authenticode 和签发者匹配。这是父进程伪造路径，本仓库不采用，也不把签名伪造成“官方启动”。MaaEnd 的 Windows 任务对 CN、B 服、Global 启动 `Launcher.exe`，对 Steam/Epic 打开商店 URI；本实现按这个安全边界选择。
- 未找到 Endfield 官方 PC 页面、Steam 元数据或上述两个启动器项目中可证明“切换到移动 UI/触控 UI”的启动参数、配置键或公开安全接口。Endfield 触控目前只能做操作系统层的鼠标/触点注入，不能证明会把 PC UI 切成移动 UI。把 Genshin/星铁那类 UI 开关移植过来需要进程内存修改或未公开运行时结构，存在反作弊检测和账号处置风险，因此本卡停止 Sparxie 的 Endfield 触控会话与 GUI 改动。
- 海报/美术不重复实现。`feat/game-art-beyond-hoyo-2` 已在做 Endfield/Arknights 的官方图源；本卡只保留“启动器可定位、启动路径可生成”的缺口。Endfield 的设置读写和官方海报在本卡没有新增实现。

## 启动资料与许可证

Collapse 的 `Hi3Helper.Plugin.Endfield` 以 `Endfield.exe` 的所在目录作为工作目录，读取用户选择的父进程来源，然后使用 `GameDecoyParentBridge` 启动游戏；该桥接器固定使用 `Games.exe` 文件名，并把挂起进程通过 `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` 设为新游戏进程的父进程。源码仓库的 Endfield 插件许可证是 MIT（`endfield-collapse-license-1efd304`），重新分发其代码需要保留许可和版权声明。本仓库没有采用它的桥接代码；它只用于确认社区实现为何能绕过启动器父进程检查，以及为什么这不符合本卡的安全边界。

MaaEnd 的 `launchgamepc.go` 将区域分为 CN、Global、Bilibili、TapTap、Steam 和 Epic。CN 使用卸载项键 `33a0a6296a20400d503c59ac0fd6341e`，Global 使用 `d9f71f86325c5b9733268efda40413c8`，B 服使用 `d957df99efbdf610162fd47c00ceaba7`；三者读取 `InstallLocation` 后运行 `Launcher.exe`。Steam URI 是 `steam://rungameid/4732690`，Epic URI 使用其清单的 `AppName`。MaaEnd 是 AGPL-3.0；本卡只使用这些行为事实，不引入其代码，来源键为 `endfield-maaend-launch-b9109d6` 和 `endfield-maaend-license-b9109d6`。

Hypergryph 官方发布页确认 PC 发行渠道；Valve 的 Steam appdetails 回答确认 Steam 应用 4732690。两者没有公开 Endfield PC 移动 UI 切换接口。Windows 官方文档说明卸载项的 `InstallLocation` 属性，以及 `RegOpenKeyExW`、`RegGetValueW`、`ExpandEnvironmentStringsW` 的读取和展开语义；这些来源支撑宿主的注册表定位代码，不支撑任何内存修改。

## 触控判定

Sparxie 现有的 HoyoTouchCore 和 ZZZ runtime 是针对各自游戏内部运行时的载荷；它们的 `touch_ui_enabled` 只是已经验证过的特定运行时 ABI，不是通用的 Windows 触控模式开关。Endfield 研究没有找到对应 ABI、命令行参数、注册表键或官方 SDK。仅注入 `SendInput`、虚拟 HID 或指针事件可以改变输入来源，但会保留 PC UI，并且无法证明游戏会把触点解释为移动布局。

若以后要重新开卡，最低证据应包括：官方或游戏公开配置中的 UI 模式定义；不修改 Endfield 进程内存的可重复切换方法；输入注入的焦点、坐标缩放、多点触控和高 DPI 验证；以及不触发反作弊的真实机器测试。当前这些证据均未核实。不要将 Collapse 的 `Games.exe` 父进程桥或任何内存补丁作为触控实现的替代方案；它们分别改变进程关系或游戏内存，风险与触控 UI 问题无关且更高。

## 本次实现

（集成说明：卸载项读取只保留 `feat/game-art-beyond-hoyo-3` 的实现，它按 `RRF_SUBKEY_WOW64xxKEY` 读取；运行时目录候选直接用 vendors 的 `hypergryph_uninstall_keys()`。下面一段描述的是本分支最初的独立实现，已被取代。）`src/sparxie/platform.cpp` 增加 Windows 卸载项读取：读取 HKLM 64 位视图、HKLM 32 位视图和当前用户 hive 的 `InstallLocation`，跳过空值、非目录和重复值，并展开 `REG_EXPAND_SZ`。`src/sparxie/runtime.cpp` 将三个 Endfield 官方启动器卸载键加入运行时目录候选，因此自定义安装和按用户安装会复用已有的 `hypergryph_launcher_exe` 版本目录查找。

已有的 `src/sparxie/host.cpp` 启动选择保持产品中立：Steam/Epic 先走商店 URI；Endfield/Arknights 的官方启动器存在时走 `Launcher.exe`；找不到启动器时保留直接 `Endfield.exe` 的回退，但这条回退可能得到 10100，且没有绕过父进程检查。它不伪造签名父进程，也不写入游戏进程内存。新增测试覆盖 Endfield CN、B 服、Global 三种 SDK 标记下均选择同一个官方 `Launcher.exe`。

## 完整性、性能与系统状态

按现有游戏适配设计，已实现：Endfield.exe 识别、CN/B 服/Global SDK 标记识别、Steam/Epic/官方启动器三类启动命令、官方启动器卸载项定位。未实现：Endfield 游戏设置读写、Endfield Sparxie 触控会话、移动 UI 切换、Endfield 新海报抓取。本卡有意不实现：父进程伪造、签名伪造、内存修改和反作弊绕过。

这条路径不是帧或输入热路径。每次宿主启动只读三条固定卸载项的两个机器视图和一个用户 hive；没有网络请求、游戏进程内存读写、循环轮询或批量分配。启动器目录按有限的版本目录遍历；复制、系统调用总量、耗时和失败率尚未测量。性能目标是生成启动命令的本地调用开销不超过 1 ms P95；本卡未做计时，也没有与 MaaEnd 或 Collapse 的启动延迟对照。

代码仍使用 Windows 原生 C++ `RegOpenKeyExW`/`RegGetValueW`，因为宿主已有 C++ Windows Backend、用户模拟和 Registry 抽象；这比新增运行时或跨进程脚本更适合读取用户 hive。Windows 源码已改动但未在 Windows VM 上构建或运行；Linux 端源码将随宿主测试构建，macOS、Android、iOS、HarmonyOS 和 Web 没有此 Backend，状态均为不适用/未核实。真实 Endfield 安装、资源授权、跨租户隔离、反作弊行为和父进程检查结果均未核实。

可兼容的外部启动协议仅放在适配层语义中：Steam URI、Epic URI、Hypergryph/GRYPHLINK 官方启动器 URI。它们的协议所有权和商店条款属于各自发布方；本仓库只生成已登记安装清单对应的 URI，不复制第三方代码。后续若做独立 Adapter，应分别验证协议版本、区域映射和商店客户端安装状态。

## 复现与使用

宿主会把检测出的 Endfield 安装显示为 `endfield`，区域为 `cn`、`bili` 或 `global`。在 Web UI 中添加该安装并选择“直接启动”即可：官方启动器安装会生成带引号的 `Launcher.exe` 命令；Steam/Epic 安装会生成对应 URI。官方启动器不在常见目录或卸载项中时，页面仍可生成 `Endfield.exe` 回退命令，但 10100 风险保持可见，建议先修复官方启动器安装。触控选项不会出现，因为当前没有安全、可证明的 Endfield 移动 UI 能力。

## 未核实项

- Windows VM 上的构建、Windows 实机启动、三种官方渠道真实安装和管理员权限交互。
- Endfield 是否存在未公开或版本相关的移动 UI 参数、注册表键或官方触控模式。
- 反作弊对直接 `Endfield.exe`、输入注入、父进程伪造的实际判定，以及账号后果。
- 复制卸载项读取的 P95/P99、启动器启动时间、CPU、内存和对手实现的公平性能数据。
- `feat/game-art-beyond-hoyo-2` 后续合入的 Endfield/Arknights 海报资产；本卡没有读取其未合入结果或重复其实现。
# 2026-11-11 更新：社区实现核对与 Endfield 触控会话

本节覆盖此前“没有可证明移动 UI 接口”的结论，并以最新公开仓库为准。读取的
Better-Endfield 为 4.1.9 发布树（提交 `26430566b98f1d97d0d156bc8e73f8671b496e1c`，
AGPL-3.0）；读取的 EndField.Fps.UnlockerIsland 为提交
`f127ac41859582fce9f8daf0b3ba9846a2dc0248`（MIT，同时 README 写有仅学习研究用途说明）。
两者只作为行为资料，宿主没有复制 AGPL 代码，也没有下载或运行云盘二进制。

## 精确行为结论

UnlockerIsland 的 `EndField.Launcher` 确实绕过官方启动器，直接以可写命令行构造
`Endfield.exe` 并调用 `CreateProcessW`。父进程就是 `EndField.Launcher.exe` 自身；没有
`PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`、`Games.exe` 父进程伪造、签名伪造或自定义环境块，
环境参数为 `nullptr`（继承默认环境）。开启注入时创建标志只有 `CREATE_SUSPENDED`，
工作目录是游戏 EXE 所在目录；它将 `-popupwindow`、`-force-d3d11` 和自定义参数附加到
命令行，然后向挂起进程写入 DLL 路径，以 `LoadLibraryW` 创建远程线程，成功后恢复线程。
因此它不是官方启动器的等价父进程，直接启动仍可能触发 10100；Collapse 的
`Games.exe` 桥是另一种父进程伪造路线，本项目不采用。

它创建的 `UnlockerIsland.SharedMem` 映射包含 magic、序列号、开关、目标 FPS、FOV 和
其它 Hook 状态；注入 DLL 解析 `UnityEngine.Application::set_targetFrameRate(System.Int32)`，
初次及共享内存变化时写入目标值。目标值没有 120 上限，UI 给出 30、45、60、120、144、
165、200、240 和自定义整数；本项目的字段范围是 10–1000，并支持会话中的热调。

Better-Endfield 的移动 UI 不是启动参数或配置文件开关，也不是云游戏 `-cloud` 模式。
它在游戏进程内通过 IL2CPP 元数据找到 `Beyond.DeviceInfo.ChangeInputType(InputType)`，
传入 `Touch`，由事件刷新 `UIStyleByState` 和触控 HUD；文档指出直接 Hook 元数据中
`get_inputType` 的死副本不会生效，必须调用真实的 `ChangeInputType`。云参数会等待专用
管道握手而停在黑屏，因此本项目不使用云参数。PC 包的真实游戏版本兼容性不是固定白名单：
Better-Endfield 4.1.9 说明按程序集、类型、方法签名动态解析，具体版本仍需实机扫描；
UnlockerIsland README 只描述其 1.0.6 发布树，未声明 Endfield 版本范围。真实版本适配仍由
运行时扫描结果决定。

## 本次宿主变更

- 分支已从 `f71cbca44aaece3158936b025010369fa7b46c47` 重放；Endfield 目录识别、卸载项
  读取和官方启动器候选仍保留，但本次删除了此前提交新增的 11 条第三方事实来源索引行。
- Endfield 目录在目录表中标记为可用触控会话，应用目录提供 `input`（`auto`、`touch`、
  `kbm`）、分辨率和显示字段；触控应用仍通过 Sparxie protocol launcher 进入会话。
- `auto` 由会话输入设备决定：触摸设备选 `Touch` UI，键鼠与手柄选 PC UI；`touch` 和
  `kbm` 是应用固定选择。目标 FPS 的 `display` 档仍由宿主读取流显示器刷新率；Endfield
  的直接启动路径继续保留商店/官方启动器命令，以便用户显式选择启动器。

## 实机尝试步骤

1. 在 Windows VM/主机关闭 Endfield 与官方启动器，确认安装目录同时包含 `Endfield.exe`、
   `GameAssembly.dll` 和 `UnityPlayer.dll`。
2. 在 Sparxie 添加 `Endfield.exe`，输入模式选“自动”，目标 FPS 选 144 或自定义大于
   120 的数值；分辨率选“跟随串流显示”。首次尝试建议先关闭全屏优化并以管理员权限运行。
3. 从触摸客户端启动该应用，观察日志中的扫描、注入和 UI 切换阶段；再从键鼠/手柄客户端
   启动同一应用，确认界面回到 PC 布局。用游戏内/串流统计核对实际帧率和分辨率。
4. 若出现 10100、登录失败、黑屏、游戏退出或账号提示，立即停止使用并保留
   `Sparxie` 与宿主日志；不要重复尝试父进程伪造。固定模式可分别选择“触控”和“PC”。

本次交付只验证了假游戏到 Endfield 特征扫描失败的边界；真实游戏启动、输入设备识别、
移动 UI 资源完整性、10100、反作弊和账号后果均未核实。
