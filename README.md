<div align="center">

<img src="docs/poster.webp" width="800" alt="Foundation Sunshine">

<br>

[![English](https://img.shields.io/badge/English-blue?style=flat-square)](README.en.md)
[![中文简体](https://img.shields.io/badge/中文简体-red?style=flat-square)](README.md)
[![Français](https://img.shields.io/badge/Français-green?style=flat-square)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-yellow?style=flat-square)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-purple?style=flat-square)](README.ja.md)

基于 [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) 的增强分支，专注于 Windows 主机的游戏串流体验

[下载发行版](https://github.com/AlkaidLab/foundation-sunshine/releases) · [使用文档](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) · [QQ 交流群](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)

</div>

---

### ░▒▓ 快速开始

1. 从 [Foundation Sunshine Releases](https://github.com/AlkaidLab/foundation-sunshine/releases) 下载适合 Windows 的安装包，安装并启动 Sunshine。发行页可能包含预发布版本，请先阅读对应版本说明。
2. 在主机浏览器打开 [https://localhost:47990](https://localhost:47990)，首次使用时创建并保存登录凭据。浏览器可能提示本地自签名证书。
3. 在控制界面添加要串流的应用，然后在 Moonlight 客户端添加主机；按客户端显示的 PIN 在 Sunshine 中完成配对。

虚拟显示器、DualSense、USB 转发和 NVIDIA 画质增强各有独立的驱动或组件要求；需要时在控制面板中查看状态并安装。更完整的步骤见[使用文档](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB)。

### ░▒▓ 核心特性

- **HDR 全链路** — 双格式编码 (PQ + HLG)・逐帧 GPU 亮度分析・HDR10+ / HDR Vivid 动态元数据・Dolby Vision 输出状态展示
- **虚拟显示器** — 深度集成 [ZakoVDD](https://github.com/qiin2333/zako-vdd)・Zako Direct 零拷贝借帧・5 种屏幕模式・多客户端 GUID 会话
- **音频增强** — 7.1.4 环绕声 (12ch)・Opus DRED 丢包恢复・持续音频流・远程麦克风・虚拟扬声器位深匹配
- **编码优化** — NVENC SDK 13.0・AMF QVBR/HQVBR/多硬件实例・编码器探测结果缓存・自适应下采样・NVENC 帧预算自适应降档
- **NVIDIA 画质增强** — 可选 RTX HDR 与 DLSS NR；DLSS NR 支持 SDR / 原生 HDR、串流中开关和处理比例调节，需兼容硬件及相应组件
- **文件夹共享** — Windows 主机目录映射・资源管理器右键共享・只读安全默认值・已配对设备授权
- **控制面板** — Tauri 2 + Vue 3 + Vite・深色模式・QR 配对・实时监控・画质增强组件管理
- **输入增强** — 全局及应用级手柄类型选择・可选虚拟 DualSense 与音频触觉・原生精密触摸板适配・虚拟鼠标驱动 (vmouse)
- **设备扩展** — 已配对客户端的 USB 转发配置・可选“所有客户端断开后结束串流”

### ░▒▓ 技术细节

<details>
<summary><b>HDR 全链路技术方案</b></summary>

#### 双格式 HDR 编码：HDR10 (PQ) + HLG 并行支持

传统串流方案仅支持 HDR10 (PQ) 绝对亮度映射，当终端设备能力不足或亮度参数不匹配时，会出现暗部细节丢失、高光截断等问题。

因此在编码层加入了 HLG（Hybrid Log-Gamma, ITU-R BT.2100）支持，采用相对亮度映射：
- **场景参考式亮度适配**：HLG 基于相对亮度曲线，显示端根据自身峰值亮度自动进行色调映射，低亮度设备上暗部细节保留显著优于 PQ
- **高光区域平滑滚降**：HLG 的对数-伽马混合传输函数在高亮区域提供渐进式滚降，避免 PQ 硬截断导致的高光色阶断裂
- **天然 SDR 向后兼容**：HLG 信号可直接被 SDR 显示器解码为标准 BT.709 画面，无需额外的色调映射处理

**逐帧亮度分析与自适应元数据生成**

在 GPU 端集成了实时亮度分析模块，通过 Compute Shader 对每帧画面执行：
- **MaxFALL / MaxCLL 逐帧计算**：实时统计帧级最大内容亮度（MaxCLL）和帧平均亮度（MaxFALL），动态注入 HEVC/AV1 SEI/OBU 元数据
- **异常值鲁棒过滤**：采用百分位截断策略剔除极端亮度像素（如高光镜面反射），防止孤立高亮点拉高全局亮度参考导致整体画面偏暗
- **帧间指数平滑**：对连续帧的亮度统计值应用 EMA（指数移动平均）滤波，消除场景切换时元数据突变引发的亮度闪烁

**完整 HDR 元数据透传**

HDR10 静态元数据（Mastering Display Info + Content Light Level）完整透传，NVENC / AMF / QSV 编码输出的码流携带符合 CTA-861 规范的完整色彩容积与亮度信息。

**HDR10+ / HDR Vivid 动态元数据注入**

在 NVENC 编码管线中，基于逐帧亮度分析结果，自动生成并注入以下动态元数据 SEI：
- **HDR10+ (ST 2094-40)**：携带场景级 MaxSCL / distribution percentiles / knee point 等色调映射参考，支持 Samsung/Panasonic 等 HDR10+ 认证电视精确色调映射
- **HDR Vivid (CUVA T/UWA 005.3)**：ITU-T T.35 注册的中国超高清视频联盟(CUVA)标准，PQ 模式下提供绝对亮度色调映射、HLG 模式下提供场景参考相对亮度色调映射，覆盖国产终端生态

</details>

<details>
<summary><b>虚拟显示器集成</b> (需 Windows 10 22H2+)</summary>

深度集成 [ZakoVDD](https://github.com/qiin2333/zako-vdd) 虚拟显示器驱动：
- 自定义分辨率和刷新率支持，10-bit HDR 色深
- **5 种屏幕组合模式**：仅虚拟屏、仅物理屏、混合模式、镜像模式、扩展模式
- IOCTL 实时通信，串流开始/结束时自动创建/销毁虚拟显示器
- 每个客户端独立绑定 VDD 会话（GUID），支持多客户端快速切换
- 无需重启的实时配置更改
- **Zako Direct 零拷贝借帧**：可直接借用 VDD 共享帧纹理，转换完成后立即归还，减少 VDD 捕获链路中的 GPU 拷贝

</details>

<details>
<summary><b>音频增强</b></summary>

- **7.1.4 环绕声 (12声道)**：Dolby Atmos 等沉浸式音频布局的完整声道映射
- **Opus DRED 深度冗余**：基于神经网络的丢包恢复，100ms 冗余窗口在网络抖动时平滑补偿
- **持续音频流**：无中断的音频流，无声时自动填充静音数据，避免音频设备反复初始化
- **虚拟扬声器自动匹配**：自动检测并匹配 16bit/24bit 等位深格式的虚拟音频设备

</details>

<details>
<summary><b>捕获与编码优化</b></summary>

**捕获管线**
- **Gamma-Aware 着色器**：根据 DXGI ColorSpace 自动选择 sRGB / 线性 Gamma 颜色转换
- **高质量下采样**：双三次 (Bicubic) 插值，支持 fast / balanced / high_quality 三档
- **动态分辨率检测**：实时感知显示器分辨率与旋转变化，编码器自适应调整
- **GPU 亮度分析**：Compute Shader 两阶段规约、P95/P99 截断、帧间 EMA 时域平滑

**NVENC**
- **SDK 13.0**：精细化码率控制与 Look-ahead
- **HDR 元数据 API**：NVENC SDK 12.2+ 原生 Mastering Display / Content Light Level 写入
- **HDR10+ / HDR Vivid SEI**：逐帧自动生成 ST 2094-40 和 CUVA T.35 动态元数据
- **SPS 码流规范**：H.264/HEVC SPS bitstream restrictions 完整写入

**AMF (AMD)**
- **QVBR / HQVBR / HQCBR**：高级码率控制，支持质量等级 UI 调节
- **低延迟可控**：AMF Low Latency、输入队列大小和 AV1 编码延迟模式均可在 WebUI 显式调整，兼顾极低延迟与驱动稳定性
- **多硬件实例编码**：支持 AMF Multi-HW Instance / Smart Access Video 相关开关，允许驱动在支持的平台上拆分编码负载

**通用**
- **编码器结果缓存**：持久化探测结果，减少后续连接时重复探测的等待
- **自适应下采样**：支持双线性 / 双三次 / 高质量三档分辨率缩放，适配 4K 主机→1080p 串流场景
- **Vulkan 编码器**：实验性 Vulkan 视频编码支持
- **无锁证书链**：`shared_mutex` 替代 mutex，消除 TLS 队列开销

</details>

<br>

---

### ░▒▓ 按需启用的功能

- **虚拟 DualSense**：在控制面板的控制器中心选择手柄类型。可设全局默认值，也可为单个应用覆盖；需要先安装可选 DualSense 组件。音频触觉还需相应 USB/IP 传输及客户端能力。组件不可用时会回退到自动手柄选择，具体状态以控制面板提示为准。
- **NVIDIA 画质增强**：在画质增强管理页配置 RTX HDR 或 DLSS NR 所需组件，再为应用开启相应功能。DLSS NR 支持串流中的即时开关和处理比例调整；界面会显示当前输出是 SDR、HDR，或已确认生效的 Dolby Vision Profile 8.1 / 8.4。输出格式取决于串流协商和设备能力。
- **USB 转发**：已配对客户端可配置运行时 USB 转发；Windows 主机需要可用的 USB/IP 传输组件。安装和设备授权请以控制面板中的状态与提示为准。

相关开发说明：[DualSense 组件](docs/windows_dualsense_component_lifecycle.md) · [NVIDIA RTX HDR 构建](docs/rtx_hdr_build.md) · [串流性能调节](docs/performance_tuning.md)

### ░▒▓ 推荐客户端

搭配以下优化版 Moonlight 客户端可获得最佳体验（激活套装属性）

- **PC** — [Moonlight-PC](https://github.com/qiin2333/moonlight-qt)（Windows · macOS · Linux）
- **Android** — [威力加强版](https://github.com/qiin2333/moonlight-vplus) · [王冠版](https://github.com/WACrown/moonlight-android)
- **iOS** — [VoidLink](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm)
- **鸿蒙** — [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

更多资源：[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

<br>

<details>
<summary><b>░▒▓ 系统要求</b></summary>

| 组件 | 最低要求 | 4K 推荐 |
|------|----------|---------|
| **GPU** | 支持硬件视频编码的 AMD / Intel / NVIDIA 显卡 | 具备适合目标分辨率、帧率及编码格式的硬件编码能力 |
| **CPU** | Ryzen 3 / Core i3 | Ryzen 5 / Core i5 |
| **RAM** | 4 GB | 8 GB |
| **系统** | Windows 10 22H2+ | Windows 10 22H2+ |
| **网络** | 5GHz 802.11ac | CAT5e 以太网 |

实际可用的编码格式、HDR 与画质增强能力取决于显卡、驱动和客户端；安装后请以 Sunshine 的编码器探测与控制面板状态为准。GPU 兼容性可参考 [NVIDIA NVENC 支持矩阵](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new)。

</details>

---

### ░▒▓ 文档与支持

[![Docs](https://img.shields.io/badge/使用文档-ff69b4?style=flat-square)](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [![LizardByte](https://img.shields.io/badge/LizardByte_文档-a78bfa?style=flat-square)](https://docs.lizardbyte.dev/projects/sunshine/latest/) [![QQ群](https://img.shields.io/badge/QQ_交流群-38bdf8?style=flat-square)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)

想帮杂鱼写代码? → [![Build](https://img.shields.io/badge/构建说明-34d399?style=flat-square)](docs/building.md) [![Config](https://img.shields.io/badge/配置指南-fbbf24?style=flat-square)](docs/configuration.md) [![WebUI](https://img.shields.io/badge/WebUI_开发-fb923c?style=flat-square)](docs/WEBUI_DEVELOPMENT.md)

<br>

<div align="center">

「 ░▒▓ 」

<a href="https://github.com/qiin2333/foundation-sunshine/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=qiin2333/foundation-sunshine&max=100" />
</a>

<br>

[![加入QQ群](https://pub.idqqimg.com/wpa/images/group.png '加入QQ群')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

<a href="https://www.star-history.com/?type=date&legend=top-left&repos=AlkaidLab%2Ffoundation-sunshine">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=AlkaidLab/foundation-sunshine&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=AlkaidLab/foundation-sunshine&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=AlkaidLab/foundation-sunshine&type=date&legend=top-left" />
 </picture>
</a>
</div>
