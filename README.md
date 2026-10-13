# Foundation Sunshine

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/readme/hero-dark.png">
  <img src="assets/readme/hero.png" width="800" alt="Foundation Sunshine：让游戏串流更优雅。低延迟、HDR、虚拟显示器。">
</picture>

[![Build](https://img.shields.io/github/actions/workflow/status/AlkaidLab/foundation-sunshine/main.yml?branch=master&label=Build)](https://github.com/AlkaidLab/foundation-sunshine/actions/workflows/main.yml) [![Latest release](https://img.shields.io/github/v/release/AlkaidLab/foundation-sunshine?include_prereleases&sort=date&label=Latest)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![Downloads](https://img.shields.io/github/downloads/AlkaidLab/foundation-sunshine/total?label=Downloads)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![GitHub Stars](https://img.shields.io/github/stars/AlkaidLab/foundation-sunshine?style=flat&label=Stars)](https://github.com/AlkaidLab/foundation-sunshine) [![License](https://img.shields.io/badge/License-GPL--3.0-blue)](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE)

简体中文 · [English](docs/readme/README.en.md) · [Deutsch](docs/readme/README.de.md) · [Français](docs/readme/README.fr.md) · [日本語](docs/readme/README.ja.md)

[官网](https://www.alkaidlab.com/) · [国内官网](https://www.alkaidlab.cn/) · [QQ 交流群](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi) · [问题反馈](https://github.com/AlkaidLab/foundation-sunshine/issues)

</div>

Foundation Sunshine 是基于 [Sunshine](https://github.com/LizardByte/Sunshine) 的免费开源远程控制软件，主要面向 Windows 特别优化，配合 Moonlight VPlus 与 Enhanced 客户端远程使用电脑桌面、应用和游戏。

从动态 HDR、屏幕校准到低延迟捕获与硬件编码，Foundation 让远程画面和操作更贴近本地体验。环绕声与麦克风透传、DualSense 原生反馈、剪贴板和文件共享也能随连接使用；独立桌面 GUI 提供更优质的用户体验，将这些原本繁琐的设置、设备组件和游戏库集中管理。

**[下载被控端（Sunshine）](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)** · **[下载控制端（Moonlight）](#控制端moonlight)** · **[安装与首次连接](#快速开始)**

[项目亮点](#项目亮点) · [功能与设置](docs/zh-CN/features.md) · [FAQ](docs/zh-CN/faq.md) · [开发文档](docs/zh-CN/development.md) · [贡献指南](docs/contributing.md)

## 项目亮点

- **画面与显示：** 场景自适应 HDR、HDR10+ / HDR Vivid 与 Dolby Vision 扩展，按屏幕亮度校准；[ZakoDisplay](https://github.com/qiin2333/zako-vdd) 无需物理显示器即可提供 HDR 虚拟屏与多种屏幕组合。
- **捕获与编码：** 支持 NVIDIA NVENC、AMD AMF、Intel Quick Sync 等编码器，并针对捕获和编码链路优化延迟与流畅度；Zako Direct 直接取得虚拟屏画面，减少捕获环节的拷贝。
- **音频与麦克风透传：** 最高 7.1.4 环绕声、PCM / AC3 / E-AC3，支持麦克风回传、静音保活与音频丢包恢复。
- **独立桌面控制面板：** 集中管理游戏库、显示器、设备组件、运行状态和更新，提供日志诊断与可选 AI 辅助。
- **输入与原生手柄：** 精确式触控板、手写笔和客户端光标；在设备中心安装 DualSense 组件后，把支持的游戏通过 USB 接入 PC 时的自适应扳机、触觉等反馈带到远程连接中。
- **桌面协作与连接管理：** 双向剪贴板、文件夹共享、USB 转发、连接中调画质、自适应码率，以及远端命令和会话控制。

具体支持条件和启用步骤见[功能与设置](docs/zh-CN/features.md)，开发者们还藏了很多小巧思等着你们来发现~

## 下载

也可以前往[官网](https://www.alkaidlab.com/)或[国内官网](https://www.alkaidlab.cn/)，按设备查找下载。

需要在两台设备上分别安装：**被控端（Sunshine）装在要被控制的电脑，控制端（Moonlight）装在你用来操作它的设备。**

### 被控端（Sunshine）

**装在要被控制的 Windows 电脑上。**

如果 Foundation Sunshine 对你有帮助，欢迎 [⭐ 点个 Star](https://github.com/AlkaidLab/foundation-sunshine) 支持项目。

前往 [最新正式发行版](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)，展开 **Assets**，下载文件名以 `WindowsInstaller.exe` 结尾的安装包。

- **首次使用：** 选择安装包，按向导完成安装。
- **需要便携部署（不推荐）：** 选择 `Portable-x64.zip`；文件名前面可能带有版本号，请确认你确实需要如多开等特殊需求时使用。
- **想体验新功能：** 查看 [预发布版本](https://github.com/AlkaidLab/foundation-sunshine/releases)，先阅读对应版本的已知问题。

开始前准备好 Windows 10 22H2 或更新版本，以及支持硬件视频编码的 AMD、Intel 或 NVIDIA 显卡。可用的画质、分辨率与帧率取决于显卡、驱动、网络和客户端。

### 控制端（Moonlight）

**装在你用来操作电脑的手机、平板或另一台电脑上。**

**推荐优先使用下列客户端。** Foundation 的 HDR、显示器管理、音频与输入增强需要控制端配合；其他客户端可能缺少相关功能，或因兼容性问题出现显示异常、连接错误。

这些客户端由基地成员用心打造，提供最优质的 Foundation Sunshine 体验，欢迎通过 **⭐ Star** 支持你使用的项目。

- **Windows / Linux：** [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **macOS：** [Moonlight macOS Enhanced](https://github.com/skyhua0224/moonlight-macos-enhanced/releases) · [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **Android / Android TV：** [Moonlight V+](https://github.com/qiin2333/moonlight-vplus/releases/latest)
- **鸿蒙：** [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

#### 合作客户端

- **VoidLink · iOS / iPadOS：** [项目介绍](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme) · [中国大陆、韩国、越南 App Store](https://apps.apple.com/app/voidlink/id6747717070) · [其他地区 App Store](https://apps.apple.com/app/voidlink-extreme/id6755103808)（商店分发收费）
- **VoidLink TestFlight Beta · 支持 Apple TV：** [加入 TestFlight 测试](https://testflight.apple.com/join/k7gs61mZ)
- **王冠版 · Android：** [下载](https://github.com/WACrown/moonlight-android/releases) · [项目介绍](https://github.com/WACrown/moonlight-android#readme)
- **包子版 · 鸿蒙：** [项目与下载](https://gitee.com/smdsbz/moonlight-ohos)

## 快速开始

### 1 安装被控端（Sunshine）

1. 在要被控制的 Windows 电脑上打开[下载页](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)。
2. 找到 **Assets**，下载以 `WindowsInstaller.exe` 结尾的文件，双击安装。安装器提供的组件可按需要勾选；准备使用虚拟屏时，保留 **Zako Display Driver** 选项。
3. 安装完成后打开 Foundation Sunshine 桌面控制面板，按新手向导选择语言、显卡和显示器。有物理屏幕时可先选择它；没有物理显示器时，先检查安装时是否已安装虚拟显示器驱动；需要补装时打开 GUI 的基地显示器设置，再选择基地显示器。
4. （**可选，不推荐**）若你更喜欢 WebUI 体验，在这台电脑的浏览器打开 [https://localhost:47990](https://localhost:47990)。如果浏览器提示证书不受信任，确认地址是这台电脑的 `https://localhost:47990` 后，通过浏览器“高级”或“更多选项”中的继续访问选项打开。
5. 首次使用按页面提示创建用户名和密码，未来可以使用它们登录 WebUI 管理界面。

### 2 安装控制端（Moonlight）

在手机、平板或另一台电脑上，按上面的[控制端（Moonlight）列表](#控制端moonlight)下载并安装客户端。这台设备用来观看画面、输入和操作。

- **Windows：** 在客户端发行页选择 Windows 安装程序；如果选择便携 ZIP（不推荐，请确认你确实需要如多开等特殊需求时使用），选择并解压再打开其中的程序，文件名前面可能带有版本号。
- **macOS：** 下载所选客户端的 DMG，打开后将应用拖到“应用程序”。V+ 预编译包仅适用于 Apple Silicon；Enhanced 可选通用包（universal），若你理解电脑所使用的架构，也可挑选对应版本。遇到首次打开拦截时，按对应项目的安装说明处理。
- **Linux：** 下载符合处理器与系统要求的 AppImage，授予可执行权限后运行。
- **Android：** 下载 APK，在下载完成提示或文件管理器中打开；按系统提示允许这次安装。
- **iOS / iPadOS、鸿蒙：** 打开对应商店链接并安装。

首次启动客户端时，如果系统询问本地网络权限，请允许访问。两台设备先连接同一局域网，被控电脑建议使用网线。

### 3 添加电脑并配对

1. 打开客户端，等待 Windows 电脑出现在列表里，然后点击它。
2. 如果没有出现，手动添加 IP：在 Windows 电脑按 `Win + R`，输入 `cmd` 并回车，再输入 `ipconfig`。找到当前使用的以太网或 WLAN 适配器下的 **IPv4 地址**，填到客户端的“添加电脑／添加主机”入口。
3. 客户端显示 PIN 后，回到 Windows 电脑的 Sunshine 管理界面，打开 **PIN 码** 页面。
4. 输入客户端显示的 PIN，点击 **发送**。看到配对成功提示后回到客户端。

这里填的是 Windows 电脑的局域网地址，例如 `192.168.1.100`，不是 `localhost`，也不是浏览器管理页面的网址。示例 IP 需要替换成你电脑的实际地址。

### 4 设置第一次连接的画质

先在 **控制端（Moonlight）** 打开设置，找到视频或画质选项，再依次设置：

| 选项 | 第一次怎么选 | 含义 |
| --- | --- | --- |
| 分辨率 | 1920×1080（1080p） | 传送画面的大小；越高越清晰，也更费网络与设备性能 |
| 帧率 | 60 FPS | 每秒画面更新次数；120 FPS 需要屏幕和两端处理能力配合 |
| 码率 | 20 Mbps；网络不稳先降到 10 Mbps | 传输数据预算，影响压缩画质与网络负载 |
| 视频编码 | 自动；兼容性问题先选 H.264 | 画面的压缩方式；HEVC / AV1 需要相应解码能力 |
| HDR | 先关闭 | 默认 SDR，确认正常后再配置 HDR |
| 音频 | 立体声 / Opus | 先确认声音，再启用多声道或其他格式 |

设置名称因客户端不同略有区别。分辨率、帧率、码率在控制端调整；Sunshine 被控端首次保持自动编码与默认捕获即可。先不要同时开启高分辨率、高帧率和所有画质增强。

### 5 连接桌面

1. 在客户端打开已配对的电脑，选择 **Desktop**。
2. 出现 Windows 桌面后，试着移动鼠标、打开一个窗口，再播放一段音频，确认画面、操作和声音都正常。
3. 如果列表中没有 Desktop，在 Sunshine 的 **应用** 页面使用 **还原内置应用**，然后刷新客户端的应用列表。这个操作会恢复默认内置项目；如已定制这些项目，请先保留配置。

先用默认画质完成这一步，再调整 HDR、分辨率或虚拟显示器。

### 6 添加游戏或应用（可选）

**批量扫描：** 在 Windows 上打开 Foundation Sunshine 桌面控制面板，进入 **应用**，点击 **扫描游戏平台库 (Steam/Epic/GOG)**。检查扫描结果，选择 **直接添加** 或 **全部添加**，然后刷新客户端列表。本地目录也可通过扫描窗口添加；浏览器管理页不能调用桌面端的本地扫描功能。

**手动添加：** 在 **应用 → 添加新应用** 中填写应用名称和 **主程序**，保存后刷新客户端列表。主程序可以填写程序路径；路径包含空格时，使用英文双引号，例如 `"D:\My Games\Game\game.exe"`。

客户端里也可以直接启动内置的 **Steam Big Picture**，再从 Steam 中选游戏。

## 功能与设置

[视频 HDR 与色彩](docs/zh-CN/features.md#hdr-与色彩) · [基地虚拟显示器](docs/zh-CN/features.md#基地虚拟显示器-zakodisplay) · [画面捕获与编码](docs/zh-CN/features.md#捕获-编码与性能) · [手柄与 DualSense](docs/zh-CN/features.md#dualsense-与虚拟手柄) · [音频与麦克风透传](docs/zh-CN/features.md#音频与麦克风透传) · [独立桌面控制面板](docs/zh-CN/features.md#桌面控制面板)

## 帮助与参与

- [FAQ 与排障](docs/zh-CN/faq.md)：卡顿、丢帧、连接、声音和 DualSense 自动滚动等问题。
- [升级说明](docs/zh-CN/upgrading.md)：正式版、测试版、配置保留与更新后检查。
- [文档目录](docs/zh-CN/README.md)：按任务找到使用与开发指南。
- [官网](https://www.alkaidlab.com/) · [国内官网](https://www.alkaidlab.cn/) · [QQ 交流群](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)。
- [提交问题](https://github.com/AlkaidLab/foundation-sunshine/issues) · [讨论与建议](https://github.com/AlkaidLab/foundation-sunshine/discussions) · [贡献指南](docs/contributing.md)。

感谢 [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine)、[Moonlight](https://moonlight-stream.org/)、[ZakoDisplay](https://github.com/qiin2333/zako-vdd)，以及开发、测试和翻译贡献者。项目遵循 [GPL-3.0 许可证](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE)。

觉得好用就点个 Star。才不是为了这个继续修 bug 的呢。


## 基地开发团队成员

| [![qiin2333](https://avatars.githubusercontent.com/u/2795904?s=80)](https://github.com/qiin2333) | [![Yundi339](https://avatars.githubusercontent.com/u/38252824?s=80)](https://github.com/Yundi339) | [![ShadowLemoon](https://avatars.githubusercontent.com/u/119576779?s=80)](https://github.com/ShadowLemoon) | [![skyhua0224](https://avatars.githubusercontent.com/u/91328606?s=80)](https://github.com/skyhua0224) |
| :---: | :---: | :---: | :---: |
| [qiin2333](https://github.com/qiin2333) | [Yundi339](https://github.com/Yundi339) | [ShadowLemoon](https://github.com/ShadowLemoon) | [skyhua0224](https://github.com/skyhua0224) |

## 其他贡献者

感谢所有参与 Foundation 和 Sunshine 上游开发、测试及维护的贡献者。

<!-- CONTRIBUTORS:START -->
<p align="center">
  <a href="https://github.com/ReenigneArcher"><img src="https://avatars.githubusercontent.com/u/42013603?v=4&amp;s=80" width="40" height="40" alt="ReenigneArcher" title="ReenigneArcher"></a>
  <a href="https://github.com/cgutman"><img src="https://avatars.githubusercontent.com/u/2695644?v=4&amp;s=80" width="40" height="40" alt="cgutman" title="cgutman"></a>
  <a href="https://github.com/apps/dependabot"><img src="https://avatars.githubusercontent.com/in/29110?v=4&amp;s=80" width="40" height="40" alt="dependabot[bot]" title="dependabot[bot]"></a>
  <a href="https://github.com/loki-47-6F-64"><img src="https://avatars.githubusercontent.com/u/3084139?v=4&amp;s=80" width="40" height="40" alt="loki-47-6F-64" title="loki-47-6F-64"></a>
  <a href="https://github.com/ns6089"><img src="https://avatars.githubusercontent.com/u/61738816?v=4&amp;s=80" width="40" height="40" alt="ns6089" title="ns6089"></a>
  <a href="https://github.com/LizardByte-bot"><img src="https://avatars.githubusercontent.com/u/108553330?v=4&amp;s=80" width="40" height="40" alt="LizardByte-bot" title="LizardByte-bot"></a>
  <a href="https://github.com/TheElixZammuto"><img src="https://avatars.githubusercontent.com/u/6505622?v=4&amp;s=80" width="40" height="40" alt="TheElixZammuto" title="TheElixZammuto"></a>
  <a href="https://github.com/psyke83"><img src="https://avatars.githubusercontent.com/u/749000?v=4&amp;s=80" width="40" height="40" alt="psyke83" title="psyke83"></a>
  <a href="https://github.com/istori1"><img src="https://avatars.githubusercontent.com/u/107304850?v=4&amp;s=80" width="40" height="40" alt="istori1" title="istori1"></a>
  <a href="https://github.com/Nonary"><img src="https://avatars.githubusercontent.com/u/27069224?v=4&amp;s=80" width="40" height="40" alt="Nonary" title="Nonary"></a>
  <a href="https://github.com/apps/github-actions"><img src="https://avatars.githubusercontent.com/in/15368?v=4&amp;s=80" width="40" height="40" alt="github-actions[bot]" title="github-actions[bot]"></a>
  <a href="https://github.com/PapyKahan"><img src="https://avatars.githubusercontent.com/u/9347985?v=4&amp;s=80" width="40" height="40" alt="PapyKahan" title="PapyKahan"></a>
  <a href="https://github.com/ABeltramo"><img src="https://avatars.githubusercontent.com/u/1122217?v=4&amp;s=80" width="40" height="40" alt="ABeltramo" title="ABeltramo"></a>
  <a href="https://github.com/FrogTheFrog"><img src="https://avatars.githubusercontent.com/u/22381748?v=4&amp;s=80" width="40" height="40" alt="FrogTheFrog" title="FrogTheFrog"></a>
  <a href="https://github.com/HomerSp"><img src="https://avatars.githubusercontent.com/u/487734?v=4&amp;s=80" width="40" height="40" alt="HomerSp" title="HomerSp"></a>
  <a href="https://github.com/kodemeister"><img src="https://avatars.githubusercontent.com/u/1043953?v=4&amp;s=80" width="40" height="40" alt="kodemeister" title="kodemeister"></a>
  <a href="https://github.com/brad-richardson"><img src="https://avatars.githubusercontent.com/u/5924884?v=4&amp;s=80" width="40" height="40" alt="brad-richardson" title="brad-richardson"></a>
  <a href="https://github.com/Doomsdayrs"><img src="https://avatars.githubusercontent.com/u/38189170?v=4&amp;s=80" width="40" height="40" alt="Doomsdayrs" title="Doomsdayrs"></a>
  <a href="https://github.com/gschintgen"><img src="https://avatars.githubusercontent.com/u/11065156?v=4&amp;s=80" width="40" height="40" alt="gschintgen" title="gschintgen"></a>
  <a href="https://github.com/Logical-sh"><img src="https://avatars.githubusercontent.com/u/5807486?v=4&amp;s=80" width="40" height="40" alt="Logical-sh" title="Logical-sh"></a>
  <a href="https://github.com/Hazer"><img src="https://avatars.githubusercontent.com/u/4046170?v=4&amp;s=80" width="40" height="40" alt="Hazer" title="Hazer"></a>
  <a href="https://github.com/diadatp"><img src="https://avatars.githubusercontent.com/u/4490283?v=4&amp;s=80" width="40" height="40" alt="diadatp" title="diadatp"></a>
  <a href="https://github.com/KuleRucket"><img src="https://avatars.githubusercontent.com/u/737724?v=4&amp;s=80" width="40" height="40" alt="KuleRucket" title="KuleRucket"></a>
  <a href="https://github.com/chewi"><img src="https://avatars.githubusercontent.com/u/35072?v=4&amp;s=80" width="40" height="40" alt="chewi" title="chewi"></a>
  <a href="https://github.com/guanzhangrtk"><img src="https://avatars.githubusercontent.com/u/44019025?v=4&amp;s=80" width="40" height="40" alt="guanzhangrtk" title="guanzhangrtk"></a>
  <a href="https://github.com/nowrep"><img src="https://avatars.githubusercontent.com/u/522831?v=4&amp;s=80" width="40" height="40" alt="nowrep" title="nowrep"></a>
  <a href="https://github.com/sitiom"><img src="https://avatars.githubusercontent.com/u/56180050?v=4&amp;s=80" width="40" height="40" alt="sitiom" title="sitiom"></a>
  <a href="https://github.com/luk1337"><img src="https://avatars.githubusercontent.com/u/6830979?v=4&amp;s=80" width="40" height="40" alt="luk1337" title="luk1337"></a>
  <a href="https://github.com/JacekJagosz"><img src="https://avatars.githubusercontent.com/u/28653965?v=4&amp;s=80" width="40" height="40" alt="JacekJagosz" title="JacekJagosz"></a>
  <a href="https://github.com/jorys-paulin"><img src="https://avatars.githubusercontent.com/u/5084309?v=4&amp;s=80" width="40" height="40" alt="jorys-paulin" title="jorys-paulin"></a>
  <a href="https://github.com/tez011"><img src="https://avatars.githubusercontent.com/u/22161268?v=4&amp;s=80" width="40" height="40" alt="tez011" title="tez011"></a>
  <a href="https://github.com/KiralyCraft"><img src="https://avatars.githubusercontent.com/u/10259742?v=4&amp;s=80" width="40" height="40" alt="KiralyCraft" title="KiralyCraft"></a>
  <a href="https://github.com/pgrunzjr"><img src="https://avatars.githubusercontent.com/u/3165068?v=4&amp;s=80" width="40" height="40" alt="pgrunzjr" title="pgrunzjr"></a>
  <a href="https://github.com/thatsysadmin"><img src="https://avatars.githubusercontent.com/u/65380846?v=4&amp;s=80" width="40" height="40" alt="thatsysadmin" title="thatsysadmin"></a>
  <a href="https://github.com/WisdomCode"><img src="https://avatars.githubusercontent.com/u/26511487?v=4&amp;s=80" width="40" height="40" alt="WisdomCode" title="WisdomCode"></a>
  <a href="https://github.com/safijari"><img src="https://avatars.githubusercontent.com/u/5191844?v=4&amp;s=80" width="40" height="40" alt="safijari" title="safijari"></a>
  <a href="https://github.com/korverdev"><img src="https://avatars.githubusercontent.com/u/84342833?v=4&amp;s=80" width="40" height="40" alt="korverdev" title="korverdev"></a>
  <a href="https://github.com/tgurr"><img src="https://avatars.githubusercontent.com/u/414984?v=4&amp;s=80" width="40" height="40" alt="tgurr" title="tgurr"></a>
  <a href="https://github.com/exetico"><img src="https://avatars.githubusercontent.com/u/3549445?v=4&amp;s=80" width="40" height="40" alt="exetico" title="exetico"></a>
  <a href="https://github.com/BigBrainAFK"><img src="https://avatars.githubusercontent.com/u/7173033?v=4&amp;s=80" width="40" height="40" alt="BigBrainAFK" title="BigBrainAFK"></a>
  <a href="https://github.com/Arbitrate3280"><img src="https://avatars.githubusercontent.com/u/26132870?v=4&amp;s=80" width="40" height="40" alt="Arbitrate3280" title="Arbitrate3280"></a>
  <a href="https://github.com/arnerak"><img src="https://avatars.githubusercontent.com/u/29173710?v=4&amp;s=80" width="40" height="40" alt="arnerak" title="arnerak"></a>
  <a href="https://github.com/detiam"><img src="https://avatars.githubusercontent.com/u/44510779?v=4&amp;s=80" width="40" height="40" alt="detiam" title="detiam"></a>
  <a href="https://github.com/dkgkdfg65"><img src="https://avatars.githubusercontent.com/u/219107372?v=4&amp;s=80" width="40" height="40" alt="dkgkdfg65" title="dkgkdfg65"></a>
  <a href="https://github.com/lns103"><img src="https://avatars.githubusercontent.com/u/49310852?v=4&amp;s=80" width="40" height="40" alt="lns103" title="lns103"></a>
  <a href="https://github.com/okkiv"><img src="https://avatars.githubusercontent.com/u/68665660?v=4&amp;s=80" width="40" height="40" alt="okkiv" title="okkiv"></a>
  <a href="https://github.com/w0utert"><img src="https://avatars.githubusercontent.com/u/30574002?v=4&amp;s=80" width="40" height="40" alt="w0utert" title="w0utert"></a>
  <a href="https://github.com/MiroKaku"><img src="https://avatars.githubusercontent.com/u/50670906?v=4&amp;s=80" width="40" height="40" alt="MiroKaku" title="MiroKaku"></a>
  <a href="https://github.com/TimmyOVO"><img src="https://avatars.githubusercontent.com/u/47740511?v=4&amp;s=80" width="40" height="40" alt="TimmyOVO" title="TimmyOVO"></a>
  <a href="https://github.com/ReeceHumphreys"><img src="https://avatars.githubusercontent.com/u/14209515?v=4&amp;s=80" width="40" height="40" alt="ReeceHumphreys" title="ReeceHumphreys"></a>
  <a href="https://github.com/mcourteaux"><img src="https://avatars.githubusercontent.com/u/845012?v=4&amp;s=80" width="40" height="40" alt="mcourteaux" title="mcourteaux"></a>
  <a href="https://github.com/Doridian"><img src="https://avatars.githubusercontent.com/u/631409?v=4&amp;s=80" width="40" height="40" alt="Doridian" title="Doridian"></a>
  <a href="https://github.com/brownj85"><img src="https://avatars.githubusercontent.com/u/35931888?v=4&amp;s=80" width="40" height="40" alt="brownj85" title="brownj85"></a>
  <a href="https://github.com/felipejfc"><img src="https://avatars.githubusercontent.com/u/285782?v=4&amp;s=80" width="40" height="40" alt="felipejfc" title="felipejfc"></a>
  <a href="https://github.com/CLF184"><img src="https://avatars.githubusercontent.com/u/57824267?v=4&amp;s=80" width="40" height="40" alt="CLF184" title="CLF184"></a>
  <a href="https://github.com/briankendall"><img src="https://avatars.githubusercontent.com/u/7917884?v=4&amp;s=80" width="40" height="40" alt="briankendall" title="briankendall"></a>
  <a href="https://github.com/abusse"><img src="https://avatars.githubusercontent.com/u/1366888?v=4&amp;s=80" width="40" height="40" alt="abusse" title="abusse"></a>
  <a href="https://github.com/perroboc"><img src="https://avatars.githubusercontent.com/u/2722773?v=4&amp;s=80" width="40" height="40" alt="perroboc" title="perroboc"></a>
  <a href="https://github.com/andygrundman"><img src="https://avatars.githubusercontent.com/u/105828?v=4&amp;s=80" width="40" height="40" alt="andygrundman" title="andygrundman"></a>
  <a href="https://github.com/CrazyStevenz"><img src="https://avatars.githubusercontent.com/u/40207026?v=4&amp;s=80" width="40" height="40" alt="CrazyStevenz" title="CrazyStevenz"></a>
  <a href="https://github.com/TrueZhuangJia"><img src="https://avatars.githubusercontent.com/u/78474576?v=4&amp;s=80" width="40" height="40" alt="TrueZhuangJia" title="TrueZhuangJia"></a>
  <a href="https://github.com/xanderfrangos"><img src="https://avatars.githubusercontent.com/u/33106561?v=4&amp;s=80" width="40" height="40" alt="xanderfrangos" title="xanderfrangos"></a>
  <a href="https://github.com/zackelia"><img src="https://avatars.githubusercontent.com/u/31748637?v=4&amp;s=80" width="40" height="40" alt="zackelia" title="zackelia"></a>
  <a href="https://github.com/zanechua"><img src="https://avatars.githubusercontent.com/u/4265429?v=4&amp;s=80" width="40" height="40" alt="zanechua" title="zanechua"></a>
  <a href="https://github.com/KawaiiZapic"><img src="https://avatars.githubusercontent.com/u/45936772?v=4&amp;s=80" width="40" height="40" alt="KawaiiZapic" title="KawaiiZapic"></a>
  <a href="https://github.com/Chang-Jin-Lee"><img src="https://avatars.githubusercontent.com/u/54494793?v=4&amp;s=80" width="40" height="40" alt="Chang-Jin-Lee" title="Chang-Jin-Lee"></a>
  <a href="https://github.com/bushshrub"><img src="https://avatars.githubusercontent.com/u/9291792?v=4&amp;s=80" width="40" height="40" alt="bushshrub" title="bushshrub"></a>
  <a href="https://github.com/RickAndTired"><img src="https://avatars.githubusercontent.com/u/53553229?v=4&amp;s=80" width="40" height="40" alt="RickAndTired" title="RickAndTired"></a>
  <a href="https://github.com/rafaelsgirao"><img src="https://avatars.githubusercontent.com/u/7544832?v=4&amp;s=80" width="40" height="40" alt="rafaelsgirao" title="rafaelsgirao"></a>
  <a href="https://github.com/radugrecu97"><img src="https://avatars.githubusercontent.com/u/39441011?v=4&amp;s=80" width="40" height="40" alt="radugrecu97" title="radugrecu97"></a>
  <a href="https://github.com/PVermeer"><img src="https://avatars.githubusercontent.com/u/23457923?v=4&amp;s=80" width="40" height="40" alt="PVermeer" title="PVermeer"></a>
  <a href="https://github.com/devusb"><img src="https://avatars.githubusercontent.com/u/4951663?v=4&amp;s=80" width="40" height="40" alt="devusb" title="devusb"></a>
  <a href="https://github.com/LXY1226"><img src="https://avatars.githubusercontent.com/u/8820178?v=4&amp;s=80" width="40" height="40" alt="LXY1226" title="LXY1226"></a>
  <a href="https://github.com/sethicis"><img src="https://avatars.githubusercontent.com/u/4684016?v=4&amp;s=80" width="40" height="40" alt="sethicis" title="sethicis"></a>
  <a href="https://github.com/SnowSwordScholar"><img src="https://avatars.githubusercontent.com/u/73566969?v=4&amp;s=80" width="40" height="40" alt="SnowSwordScholar" title="SnowSwordScholar"></a>
  <a href="https://github.com/Max-Might"><img src="https://avatars.githubusercontent.com/u/119780?v=4&amp;s=80" width="40" height="40" alt="Max-Might" title="Max-Might"></a>
  <a href="https://github.com/zoeyjodon"><img src="https://avatars.githubusercontent.com/u/76182954?v=4&amp;s=80" width="40" height="40" alt="zoeyjodon" title="zoeyjodon"></a>
  <a href="https://github.com/skryvel"><img src="https://avatars.githubusercontent.com/u/120495777?v=4&amp;s=80" width="40" height="40" alt="skryvel" title="skryvel"></a>
  <a href="https://github.com/sirius128"><img src="https://avatars.githubusercontent.com/u/68372698?v=4&amp;s=80" width="40" height="40" alt="sirius128" title="sirius128"></a>
  <a href="https://github.com/qingm36"><img src="https://avatars.githubusercontent.com/u/76875055?v=4&amp;s=80" width="40" height="40" alt="qingm36" title="qingm36"></a>
  <a href="https://github.com/manteuffel723"><img src="https://avatars.githubusercontent.com/u/40317957?v=4&amp;s=80" width="40" height="40" alt="manteuffel723" title="manteuffel723"></a>
  <a href="https://github.com/jensaymoo"><img src="https://avatars.githubusercontent.com/u/45825226?v=4&amp;s=80" width="40" height="40" alt="jensaymoo" title="jensaymoo"></a>
  <a href="https://github.com/jcj83429"><img src="https://avatars.githubusercontent.com/u/5402600?v=4&amp;s=80" width="40" height="40" alt="jcj83429" title="jcj83429"></a>
  <a href="https://github.com/iMakeSoftware"><img src="https://avatars.githubusercontent.com/u/20089877?v=4&amp;s=80" width="40" height="40" alt="iMakeSoftware" title="iMakeSoftware"></a>
  <a href="https://github.com/exalented"><img src="https://avatars.githubusercontent.com/u/9328864?v=4&amp;s=80" width="40" height="40" alt="exalented" title="exalented"></a>
  <a href="https://github.com/everything411"><img src="https://avatars.githubusercontent.com/u/32010059?v=4&amp;s=80" width="40" height="40" alt="everything411" title="everything411"></a>
  <a href="https://github.com/brycerocky"><img src="https://avatars.githubusercontent.com/u/56776312?v=4&amp;s=80" width="40" height="40" alt="brycerocky" title="brycerocky"></a>
  <a href="https://github.com/aseligmann"><img src="https://avatars.githubusercontent.com/u/40468411?v=4&amp;s=80" width="40" height="40" alt="aseligmann" title="aseligmann"></a>
  <a href="https://github.com/ahmadalli"><img src="https://avatars.githubusercontent.com/u/2167301?v=4&amp;s=80" width="40" height="40" alt="ahmadalli" title="ahmadalli"></a>
  <a href="https://github.com/aetheryx"><img src="https://avatars.githubusercontent.com/u/25572140?v=4&amp;s=80" width="40" height="40" alt="aetheryx" title="aetheryx"></a>
  <a href="https://github.com/notentered"><img src="https://avatars.githubusercontent.com/u/1017032?v=4&amp;s=80" width="40" height="40" alt="notentered" title="notentered"></a>
  <a href="https://github.com/360sym"><img src="https://avatars.githubusercontent.com/u/63459534?v=4&amp;s=80" width="40" height="40" alt="360sym" title="360sym"></a>
  <a href="https://github.com/d-s-x"><img src="https://avatars.githubusercontent.com/u/5136335?v=4&amp;s=80" width="40" height="40" alt="d-s-x" title="d-s-x"></a>
  <a href="https://github.com/Xythus"><img src="https://avatars.githubusercontent.com/u/1338909?v=4&amp;s=80" width="40" height="40" alt="Xythus" title="Xythus"></a>
  <a href="https://github.com/Crashdummyy"><img src="https://avatars.githubusercontent.com/u/15139403?v=4&amp;s=80" width="40" height="40" alt="Crashdummyy" title="Crashdummyy"></a>
  <a href="https://github.com/cgarst"><img src="https://avatars.githubusercontent.com/u/125395?v=4&amp;s=80" width="40" height="40" alt="cgarst" title="cgarst"></a>
  <a href="https://github.com/apps/copilot-swe-agent"><img src="https://avatars.githubusercontent.com/in/1143301?v=4&amp;s=80" width="40" height="40" alt="Copilot" title="Copilot"></a>
  <a href="https://github.com/CodyManess"><img src="https://avatars.githubusercontent.com/u/47895180?v=4&amp;s=80" width="40" height="40" alt="CodyManess" title="CodyManess"></a>
  <a href="https://github.com/Clutchnp"><img src="https://avatars.githubusercontent.com/u/105556048?v=4&amp;s=80" width="40" height="40" alt="Clutchnp" title="Clutchnp"></a>
  <a href="https://github.com/cfalas"><img src="https://avatars.githubusercontent.com/u/34488895?v=4&amp;s=80" width="40" height="40" alt="cfalas" title="cfalas"></a>
  <a href="https://github.com/cfajardo"><img src="https://avatars.githubusercontent.com/u/104530852?v=4&amp;s=80" width="40" height="40" alt="cfajardo" title="cfajardo"></a>
  <a href="https://github.com/cazzoo"><img src="https://avatars.githubusercontent.com/u/1876754?v=4&amp;s=80" width="40" height="40" alt="cazzoo" title="cazzoo"></a>
  <a href="https://github.com/caveat-makes-software"><img src="https://avatars.githubusercontent.com/u/59033732?v=4&amp;s=80" width="40" height="40" alt="caveat-makes-software" title="caveat-makes-software"></a>
  <a href="https://github.com/cathyjf"><img src="https://avatars.githubusercontent.com/u/508485?v=4&amp;s=80" width="40" height="40" alt="cathyjf" title="cathyjf"></a>
  <a href="https://github.com/gamozolabs"><img src="https://avatars.githubusercontent.com/u/13505090?v=4&amp;s=80" width="40" height="40" alt="gamozolabs" title="gamozolabs"></a>
  <a href="https://github.com/BeeLeDev"><img src="https://avatars.githubusercontent.com/u/60373320?v=4&amp;s=80" width="40" height="40" alt="BeeLeDev" title="BeeLeDev"></a>
  <a href="https://github.com/angelnu"><img src="https://avatars.githubusercontent.com/u/4406403?v=4&amp;s=80" width="40" height="40" alt="angelnu" title="angelnu"></a>
  <a href="https://github.com/amarshall"><img src="https://avatars.githubusercontent.com/u/153175?v=4&amp;s=80" width="40" height="40" alt="amarshall" title="amarshall"></a>
  <a href="https://github.com/Aaron-Rumpler"><img src="https://avatars.githubusercontent.com/u/80610725?v=4&amp;s=80" width="40" height="40" alt="Aaron-Rumpler" title="Aaron-Rumpler"></a>
  <a href="https://github.com/kelvie"><img src="https://avatars.githubusercontent.com/u/5893?v=4&amp;s=80" width="40" height="40" alt="kelvie" title="kelvie"></a>
  <a href="https://github.com/KarlVogel"><img src="https://avatars.githubusercontent.com/u/4101575?v=4&amp;s=80" width="40" height="40" alt="KarlVogel" title="KarlVogel"></a>
  <a href="https://github.com/justindhillon"><img src="https://avatars.githubusercontent.com/u/145078271?v=4&amp;s=80" width="40" height="40" alt="justindhillon" title="justindhillon"></a>
  <a href="https://github.com/Tea23"><img src="https://avatars.githubusercontent.com/u/1298028?v=4&amp;s=80" width="40" height="40" alt="Tea23" title="Tea23"></a>
  <a href="https://github.com/jhgarner"><img src="https://avatars.githubusercontent.com/u/7400280?v=4&amp;s=80" width="40" height="40" alt="jhgarner" title="jhgarner"></a>
  <a href="https://github.com/insanemal"><img src="https://avatars.githubusercontent.com/u/215062?v=4&amp;s=80" width="40" height="40" alt="insanemal" title="insanemal"></a>
  <a href="https://github.com/Calinou"><img src="https://avatars.githubusercontent.com/u/180032?v=4&amp;s=80" width="40" height="40" alt="Calinou" title="Calinou"></a>
  <a href="https://github.com/hgaiser"><img src="https://avatars.githubusercontent.com/u/716138?v=4&amp;s=80" width="40" height="40" alt="hgaiser" title="hgaiser"></a>
  <a href="https://github.com/Han-cy830"><img src="https://avatars.githubusercontent.com/u/279452837?v=4&amp;s=80" width="40" height="40" alt="Han-cy830" title="Han-cy830"></a>
  <a href="https://github.com/hadicharara"><img src="https://avatars.githubusercontent.com/u/46264409?v=4&amp;s=80" width="40" height="40" alt="hadicharara" title="hadicharara"></a>
  <a href="https://github.com/c3m3gyanesh"><img src="https://avatars.githubusercontent.com/u/11159228?v=4&amp;s=80" width="40" height="40" alt="c3m3gyanesh" title="c3m3gyanesh"></a>
  <a href="https://github.com/himekifee"><img src="https://avatars.githubusercontent.com/u/65988663?v=4&amp;s=80" width="40" height="40" alt="himekifee" title="himekifee"></a>
  <a href="https://github.com/theofficialgman"><img src="https://avatars.githubusercontent.com/u/28281419?v=4&amp;s=80" width="40" height="40" alt="theofficialgman" title="theofficialgman"></a>
  <a href="https://github.com/ekchang"><img src="https://avatars.githubusercontent.com/u/10266208?v=4&amp;s=80" width="40" height="40" alt="ekchang" title="ekchang"></a>
  <a href="https://github.com/e-dong"><img src="https://avatars.githubusercontent.com/u/16567905?v=4&amp;s=80" width="40" height="40" alt="e-dong" title="e-dong"></a>
  <a href="https://github.com/Gara-Dorta"><img src="https://avatars.githubusercontent.com/u/1507774?v=4&amp;s=80" width="40" height="40" alt="Gara-Dorta" title="Gara-Dorta"></a>
  <a href="https://github.com/ehabajmal"><img src="https://avatars.githubusercontent.com/u/33014707?v=4&amp;s=80" width="40" height="40" alt="ehabajmal" title="ehabajmal"></a>
</p>
<!-- CONTRIBUTORS:END -->

[查看贡献记录](https://github.com/AlkaidLab/foundation-sunshine/graphs/contributors)
