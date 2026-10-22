# Linux 移植遗留差距清单

- **基线**：`linux-migration` 分支（pkgrel 66，2026-10-22；已并入上游 `origin/master` `7a0995e2`，
  上一个同步点是 `ed415026`）
- **对照**：fork Windows 版（AlkaidLab master `3d76d35d`）的全部增强功能
- **用途**：待办工作清单。每项含现状钩子（代码位置）、技术路径、预估工作量、验收标准
- **约定**：🔶 部分实现（有偏差）｜○ 未实现但可行｜⛔ 本质不可移植
- **已完成项**：见 `LINUX_MIGRATION_REPORT.md` §六，本文档不再复述

---

## 总览

核心体验链路已全部对齐：虚拟显示器（原生 EDID 后端 + 配置全组合模式表 + prep 模式 + 独占还原 +
残留 adopt）、物理显示器 display_device（kscreen 分辨率/HDR/主屏/拓扑 + 持久化还原）、HDR 三件套
（静态透传 / HDR10+ 动态 / DV P8.1 RPU / Vivid）、剪贴板文本双向、远程麦克风、ABR 前台检测
（KDE + niri）、托盘/向导/打包。

剩余：**7 项部分实现**（§一）、**11 项可行未做**（§二）、**9 项本质不可移植**（§三）。
另有**有意保留的差异**（§四，决策记录）、**上游新特性的 Linux 缺口**（§五），以及
**2026-10-22 上游同步（`ed415026..7a0995e2`，33 个提交）新引入的缺口**（§七）。

---

## 一、🔶 部分实现（有已知偏差）

### 1.1 虚拟显示器 per-client 隔离

- **现状**：客户端切换时**销毁重建** VDD（`src/display_device/session.cpp` 的 `rebuild_old_vdd_id`
  链路），单虚拟屏共享；Windows 是多实例并存，每客户端独立 GUID（`vdd_utils::generate_client_guid`）。
- **差距**：两个客户端无法同时各用各的虚拟屏。
- **若要做**：Linux 后端需支持多个 EDID override 连接器并存（每客户端占一个空闲物理连接器），
  `create_vdd_monitor` 带 GUID 参数化显示名。受限于机器上的空闲连接器数量。
- **工作量**：高。**验收**：两客户端同时各串各的虚拟屏互不干扰。

### 1.2 HDR 亮度分析精度

- **现状**：CPU 亮度直方图，PQ 亮度域（`src/video.cpp:396` `analyze_pq_luma_frame`）。
- **差距**：Windows 是 D3D11 计算着色器逐像素 max(R,G,B)（scRGB）；亮度 ≤ maxRGB，饱和纯色场景
  maxSCL 略偏低。消费端已按近似处理（99 分位当 maxSCL）。
- **若要做**：CUDA kernel（nvenc 会话有 CUDA hwdevice）或 Vulkan compute 对 YUV→RGB 逐像素求
  maxRGB。
- **工作量**：高。**验收**：纯红/纯蓝测试图 maxSCL 接近真实值。实际观感收益存疑，优先级低。

### 1.3 剪贴板非文本类 + 非 KDE provider

- **现状**：`src/clipboard_host.cpp` 仅处理 KIND_TEXT（klipper D-Bus 只支持文本读写）；
  KIND_PNG / KIND_FILE_OFFER 的**主机写入侧**缺失；非 KDE 桌面无任何 provider。
  大文本的 KIND_REF blob 回退**已实现**。
- **若要做**：
  - 图片：写 wlr-data-control Wayland 协议客户端（KWin 支持；wayland-scanner 生成协议码，构建已有
    wayland 依赖）。SELECTION 源设 PNG+text/uri-list。
  - 大文件：接 file-offer 语义（blob store 已跨平台）。
  - 非 KDE：X11 XFixes selection 监听（项目已链接 X11）。
- **工作量**：中（wlr-data-control 客户端是大头）。**验收**：手机复制截图 → 主机粘贴为图片。

### 1.4 display_device 复制拓扑 + 非 KDE 桌面

- **现状**：`src/platform/linux/display_device.cpp` 拓扑每组单设备；`set_topology` 对多设备组直接
  拒绝。模式/HDR/主屏/拓扑读写全部依赖 kscreen-doctor（KDE Plasma）；其他桌面查询返回空 →
  退化为 no-op 行为（不破坏，但无功能，有一条 warning）。
- **若要做**：
  - 复制拓扑：kscreen-doctor 无 replication 设置命令 → 需走 KWin 脚本（同 ABR 前台检测的加载模式）
    或 libkscreen D-Bus。
  - GNOME：Mutter DisplayConfig D-Bus（`org.gnome.Mutter.DisplayConfig`，SetMonitorsConfig 支持
    mode/scale/primary），实现独立 backend 类按 `XDG_CURRENT_DESKTOP` 分发。
- **工作量**：复制拓扑中；GNOME 后端中-高。**验收**：GNOME 会话下分辨率/HDR 切换可用。

### 1.5 帧管线契约（frame_contract）

- **现状**：策略层跨平台（`src/platform/frame_contract.cpp`），只有 Windows 采集端消费。
- **差距**：Linux 采集路径不消费契约（帧时间戳在 avcodec 路径已有，但契约的"合成 HDR 前置滤波"等
  阶段依赖 pre_encode_filter）。
- **结论**：随 pre_encode_filter 一起评估（§三 的伴生项），单独无意义。

### 1.6 AI API Key 加密存储

- **现状**：Linux 明文环境变量 `SUNSHINE_LLM_API_KEY`（`src/ai/credential_store.cpp`）；Windows 用
  DPAPI。
- **若要做**：libsecret（gnome-keyring/kwallet 后端）——KDE 环境有 kwallet 的 secret service 兼容层。
  编译依赖 `libsecret`，可选发现（找不到回退环境变量）。
- **工作量**：低。**验收**：Key 存入 keyring，进程列表/env 里看不到明文。

### 1.7 音频设备位深匹配

- **现状**：虚拟 sink 固定 `PA_SAMPLE_FLOAT32`（`src/platform/linux/audio.cpp`）。Windows 用
  PolicyConfig COM 按客户端位深建设备避免 WASAPI 重采样。
- **实际影响**：PipeWire 服务端自适应转换，质量无损；仅多一次转换的 CPU 开销（可忽略）。
- **结论**：**建议关闭**（不处理），除非实测发现问题。

---

## 二、○ 未实现但可行（按性价比排序）

### 2.1 剪贴板图片 + 大文件（中）⭐ 推荐下一个

- 见 §1.3 的路径。拆两步：先 wlr-data-control 图片双向（PNG），再 file-offer。
- **验收**：手机复制截图 → 主机 Ctrl+V 得到图片；主机复制图片 → 手机粘贴。

### 2.2 USB/IP 主机（中高）

- **现状**：`src/remote_usb/remote_usb_host_controller.cpp` 非 Windows 直接返回
  `usbip_host_backend::unsupported`。
- **路径**：Linux 是 usbip 的内核原生平台（`usbip-host` 内核模块 + `usbipd` 用户态工具）。把
  unsupported 后端替换为：枚举可导出的 USB 设备（sysfs busid）→ `usbip bind` → TCP 3240 端口复用
  现有 reverse tunnel。fork 的协议层（`loopback_usbip_bridge`、`reverse_tunnel_service`）是跨平台源码。
- **前置**：内核 `usbip_host` 模块（`usbip` 包含用户态工具）。
- **验收**：客户端挂载主机 U 盘/手柄，读写正常。

### 2.3 GNOME Mutter 显示后端（中-高）

- 见 §1.4。独立 backend 类 + `XDG_CURRENT_DESKTOP` 分发（`vdd_utils::hint_primary_output` 已有
  分发骨架）。注意 `display_device.cpp` 当前所有读写函数都隐式假设 kscreen 存在。
- **验收**：GNOME Wayland 会话下客户端"优化游戏设置"能切分辨率/HDR 并还原。

### 2.4 DSU 运动数据服务器（低中）

- **现状**：`src/platform/windows/dsu_server.cpp`（DSU/cemuhook UDP 协议，497 行 asio UDP，
  除 winsock include 外与平台无关）只在 `src/platform/windows/input.cpp:555-572` 挂载；
  喂数据的入口是 `dsu_server->send_motion_data(controller_id, ...)`（同文件 :2507/:2514）。
- **路径修正**：原以为 inputtino 的 DS5 能提供加速度/陀螺仪数据源，实际
  `third-party/inputtino/include/inputtino/input.hpp:417` 是 `set_motion(MOTION_TYPE, x, y, z)`
  ——**注入**方向，不是采集。所以 DSU 在 Linux 上没有可广播的源。真要做，必须先落地
  §2.7 的第一步：用 `/dev/hidraw` 读物理 DualSense 的 input report 0x01/0x31 取 IMU，
  再复用 Windows 那份协议实现（把它从 `src/platform/windows/` 挪成共享源 + 平台门控）。
- **验收**：Cemu/模拟器识别到运动输入（需 §2.7 的 hidraw 采集先就位）。

### 2.5 非 KDE 前台检测（低）—— niri 已覆盖，其余待做

- **已完成**：niri（轮询 `niri msg --json focused-window`，KWin 优先、niri 兜底）。
- **仍缺**：X11：XFixes + `_NET_ACTIVE_WINDOW`/`_NET_WM_PID`（X11 库已链接）；wlroots 系
  （sway/Hyprland）：`ext-foreign-toplevel-list` / wlr-foreign-toplevel-management。两者都只需接入
  `src/platform/linux/foreground_app.cpp` 的同一 producer 选择与缓存。
- **验收**：Hyprland/X11 会话下 ABR 日志出现前台切换。

### 2.6 AI Key libsecret 后端（低）

- 见 §1.6。

### 2.7 DS5 本地触觉播放 sidecar（高）

- Windows `src/platform/windows/ds5/ds5_sidecar_client.cpp` + `mic_write` 的触觉 PCM 在主机直连手柄上播放。
- **路径**：`/dev/hidraw` 直写 DualSense 输出报告（udc3 打开 + 触觉报告格式），权限走 udev tag。
  基础 rumble 已由 inputtino 覆盖，缺的是 authored IR PCM 高保真回放。
- **验收**：直连 DS5 玩支持触觉的游戏，波形与 Windows 版一致。

### 2.8 NVENC SDK 直连（高，建议搁置）

- HDR10+/DV 已完成，avcodec 路径功能面已齐。直连的剩余增益：fork 的细粒度码控、lookahead、
  多硬件实例。Vivid 的 T.35 手写序列化器反而是直连的*负资产*（avcodec 不需要它）。
- **建议**：搁置，等实际画质/延迟瓶颈出现再评估。

### 2.9 Tauri 控制面板选择性移植（高，收益低）

- 面板约 20 个系统模块仅 Windows 实现（vdd/vigem/vmouse/rtss/hwinfo(WMI)/elevation/注册表自启/
  dualsense elevated/shell 右键菜单等）。WebUI 已覆盖大部分。
- **若有需求**，只值得挑：QR 配对、实时监控（→ `nvidia-smi`/`amdgpu_top`/PipeWire 统计）、
  文件右键共享（KDE ServiceMenu 更自然）。
- **建议**：搁置；文件右键共享若想要，走 KDE ServiceMenu（低工作量）单独立项。

### 2.10 niri / wlr-output-management 显示后端（中）— 第二目标环境

- **背景**：目标是 **KDE + niri** 双环境、通用方案优先。VDD 本身与合成器无关（DRM 层 + niri 默认
  自动启用新输出），但 **display_device 的模式/HDR/拓扑**全部经 kscreen-doctor → niri 下
  `query_outputs()` 为空，客户端的模式/HDR 请求被忽略（降级为既有 no-op + warning）。
- **已完成**：前台检测的 niri producer、VDD 输出的合成器无关启用
  （`src/platform/linux/compositor_output.{h,cpp}`）、非 Plasma 会话不再被 kscreen-doctor 阻塞
  （`src/platform/linux/kscreen_backend.{h,cpp}`）。
- **待做**：`query_outputs()` / 模式应用 / HDR 目前只有 kscreen 实现。niri 需用其 IPC
  （`niri msg outputs` / `output <name> mode|on|off|scale|position`），通用兜底用
  wlr-output-management（`wlr-randr`，覆盖 sway/Hyprland 等）。**HDR 是否有 IPC/配置项需在 niri 上
  确认，无则报 unknown 而不是猜**。
- **验收**：niri 会话下客户端分辨率请求能作用到目标输出。**注意**：本机未装 niri，命令语法必须在
  niri 环境实测；一律先做能力探测，探测失败必须保持现有降级路径（不得影响 KDE 与非 KDE 现状）。

### 2.11 VAAPI 的 YUV 4:4:4（低，NVENC 已解禁）

- **背景**：`src/platform/linux/vaapi.cpp` 的 `get_va_profile()` 已能把 `AV_PROFILE_HEVC_REXT` +
  8/10-bit 映射到 `VAProfileHEVCMain444`/`Main444_10`，且 `is_va_profile_supported()` 会经
  `vaQueryConfigProfiles()` 查驱动——这条 444 链路**代码已备**，只是 vaapi encoder 条目
  （`src/video.cpp` 的 `#ifdef __linux__` 段）的 444 格式仍为 `AV_PIX_FMT_NONE` 且无
  `YUV444_SUPPORT`，走不到。
- **做法**：条目 444 填 `AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10`（hevc_vaapi 收平面格式）+ 旗标，
  然后靠真实试编码探测兜底；`va_t::set_frame()` 的 VA surface 导出还要从写死的 2 层
  （`prime.num_layers != 2` 拒绝）泛化到 3 平面 444 surface。
- **收益/风险**：Intel/AMD iGPU 的 HEVC 444（h264 无 VAAPI 444 profile，天然只有 HEVC）；NVIDIA 上
  nvidia-vaapi-driver 无编码入口，探测自然拒绝。**本机（NVIDIA）无收益、无法实测**。

---

## 三、⛔ 本质不可移植（含原因与 Linux 对应物）

| Windows 功能 | 不可移植原因 | Linux 对应物 / 状态 |
|---|---|---|
| Zako Direct 零拷贝借帧 | D3D11 共享纹理，无 Linux 对等机制 | KMS 捕获直读 DRM plane（功能面已覆盖） |
| WGC 捕获 | Windows Graphics Capture API | kmsgrab ✅ |
| rtx_hdr / TrueHDR 链 | RTX Video HDR = NVIDIA Windows 驱动 API + D3D11 合成管线 | ⛔ 无；且 `rtsp.cpp` 的 TrueHDR PQ 链路整体 Windows 门控 |
| AMF 硬件编码 | AMD Windows 运行时 | VAAPI（上游已有；N 卡不适用） |
| HLG 捕获源 → DV P8.4 | 内核 `HDR_OUTPUT_METADATA` UAPI 只定义 PQ/SDR | ⛔ 等内核演进；P8.1 已覆盖 DV 主流 |
| ZakoVDD 硬件光标导出通道 | Windows 驱动私有通道 | KMS 光标平面（合成器渲染光标进捕获）✅ |
| 触摸键盘自动唤起 | Windows 任务栏注册表机制 | 头文件本身非 Windows 即 no-op，无需处理 |
| nvprefs 驱动设置 | NVIDIA Windows 控制面板 API | nvidia-drm modeset 参数（系统层已配置）✅ |
| 服务包装器 / UAC 提权 / Win 深色模式 / GFE 兼容 | Windows 平台概念 | systemd 用户服务 ✅ / 无此概念 ✅ |

---

## 四、有意保留的差异（决策记录，非缺口）

以下项经复核判定**保持现状**，理由记录在此，便于日后按需推翻：

- **空容器契约**：Linux 的 `get_current_*` 以"空集合 = 全部已启用输出"、setter 以"空 map = 无需处理
  （true）"为准；Windows 分别返回 `{}`/false。树内**没有调用方**传空集合查询，而 setter 的空 map
  语义被还原路径依赖（"没有要还原的" → 不能算失败）。改成 Windows 语义只会把无操作变成失败。
- **麦克风契约边界**：null samples → 0（Windows -1）、重复 init → 0（Windows -1）、返回字节数 2 B/帧
  （Windows 端点相关 4 B）、缓冲属性留服务端默认（Windows 显式 100 ms）。消费者 `stream.cpp` 只区分
  `==0`（丢帧计数）与 `<0`（重初始化）；改成 -1 反而会触发无意义的重初始化循环。
- **日志语言**：同一事件 Windows 中文 / Linux 英文。Linux 端整个日志面是英文，混语种更差；**面向用户
  的 UI 文案走托盘 i18n（中/英/日）**。日志字符串不参与行为或线协议契约。
- **"blank HDR toggle"**：Windows 在切换 HDR 前把新启用显示器先切到相反状态、等 2333 ms 再设最终值，
  是 Windows 显示栈（IDD/VDD）的"颜色发白"清理手段；Linux 的 VDD 是真实 DRM 连接器、无对应症状，
  移植只会给每次新启用显示器加 2.3 s 延迟。如实测出现同症状再补。
- **前台 exe 语义**：Linux 报 Wayland app class、Windows 报进程映像名（含 `.exe`）。Wayland 下没有
  等价的"进程映像名"来源（合成器只暴露 app_id/class），改成"猜进程名"会引入不可靠映射。
- **直方图估计器**：Linux 精确 1024 码直方图 vs Windows 256 bin 采样——语义一致、数值不同，Linux 更
  精确。
- **剪贴板线协议常量的跨语言同步**：C++ 侧已收敛到 `src/clipboard_bridge.h`（版本/kind/帧头/内联阈值/
  TTL），Rust agent（`clipboard.rs`）是权威参考且无法包含 C++ 头文件。为 5 个协议常量引入代码生成
  步骤不划算；C++ 镜像由 `tests/unit/test_clipboard_wire.cpp` 的编解码测试守住。
- **显示设置失败时的返回值**：合成器不可用时 display_device 保留 success 结果（Linux 上这是会话的持久
  属性，返回失败会让所有非 KDE 会话进入无法修复的 deferred-retry），但日志提到 warning 并明确
  "显示设置未被应用、串流按当前布局继续"。

---

## 五、上游新特性在 Linux 的缺口

上游 `ed415026`（并入本分支的 50 个提交）带来的新特性在 Linux 侧的现状。这些**不是 fork 的 Windows
增强**，而是上游自己的新功能。均已确认**不会破坏 Linux 构建**（共享层能编译，平台后端缺失处以
`#ifdef _WIN32` 门控或探测拒绝兜底）。

### 5.1 ○ PyroWave 编码后端仅 Windows（高）

- **现状**：共享层完整编译——`src/pyrowave/{types,config,capabilities,gpu_interop,packet,packetizer,
  dynamic_hdr,session,runtime}.cpp` 都在 `cmake/compile_definitions/common.cmake`，
  `tests/unit/test_pyrowave.cpp` 在 Linux 跑通。缺的是**平台后端**：
  `src/platform/windows/pyrowave/{cpu_encoder,gpu_encoder,transport}.cpp` 只在 `windows.cmake`；
  `video.cpp` 的 PyroWave 编码派发整块位于 `#ifdef _WIN32`。
- **Linux 行为**：本分支从未把 PyroWave 编码器注册进编码器表，客户端不会协商到它；即便协商到，
  `encode()` 的派发链会落到末尾 `return -1`，会话按既有的"PyroWave 会话初始化失败 → shutdown"
  路径结束，不会静默出坏流。
- **若要做**：Linux 需要 CPU 后端（x264/x265 软件编码 + 块级封装，`cpu_encoder.cpp` 无 D3D 依赖，
  是最可行的一块）与传输层（`transport.cpp` 的 socket/重建路径）。GPU 互操作（`gpu_interop.cpp`）
  在 Linux 需对应 CUDA/EGL 纹理导入。
- **工作量**：中-高（CPU 后端中）。**验收**：Linux 客户端协商 PyroWave 后能出画且 RS 重传生效。

### 5.2 ○ 动态 RS-FEC 已随共享层落地（已基本完成）

- **现状**：`src/streaming/fec.cpp`（动态 RS 保护 + 客户端覆盖）与 `src/streaming/bitrate.cpp`
  （会话码率预算，含音频/开销扣除与 FEC 折算）都在共享列表里，Linux 与 Windows 同源编译。
  `video.cpp::set_bitrate` 已对齐该预算模型——**Linux 的 FFmpeg avcodec 动态码率因此走统一的预算链路**。
- **剩余**：`perf_recorder`、`widget_http` 同样在共享列表（Linux 可用）。真正平台相关的是 NVENC 原生
  帧预算守卫（§5.3）。
- **验收**：Linux 客户端改码率时日志出现 `Applied video budget: total=... encoder=... FEC=...`。

### 5.3 ○ NVENC 帧预算守卫对 Linux 无意义（决策）

- **现状**：`src/nvenc/frame_budget.{h,cpp}` 是**纯函数**（无 SDK、无硬件访问），已加入
  `PLATFORM_TARGET_FILES` 以满足共享 `confighttp.cpp` 的链接需求。但**发布者**
  `publish_frame_budget_report()` 的调用点在 `src/nvenc/common_impl/nvenc_base.cpp`，而整个
  `src/nvenc/` 原生 SDK 路径（含 `common_impl/`）只在 Windows 编译。
- **Linux 行为**：`GET /api/config` 不含 `active_nvenc_frame_budget` 块——Linux 的 NVENC 是 FFmpeg
  的 `*_nvenc` 封装，不经过原生 SDK 的 `create_encoder()`，没有"配置预设 vs 有效预设"的概念。
  这是**正确行为**，不是缺口。
- **验收**：Linux `/api/config` 不出现该块，且不报错。

### 5.4 ⛔ DLSS NR / RTX HDR / Game Bar 小部件等（Windows 专有 SDK）

- **现状**：`sunshine_dlssnr_adapter` / `sunshine_rtx_video_adapter`
  （`cmake/dependencies/dlssnr_adapter.cmake`、`rtx_video_adapter.cmake`）与
  `src/platform/windows/image_enhancement/**` 全部 Windows-only；`src/widget_http.cpp` 共享层在 Linux
  编译，但其 UWP 端点骨架是 Windows 概念。
- **Linux 对应物**：图像增强在 Linux 无等价物（CUDA/Vulkan 自研滤波属独立大特性）；Game Bar 小部件与
  Windows 外壳绑定，无移植意义。

### 5.5 ○ "下线显示模式重映射"重构（已完成，无需跟进）

- **现状**：上游 `8b905e2c` 删除了 `src/display_device/parsed_config.cpp` 的
  `remap_display_modes_if_needed()` 与配置键 `display_mode_remapping`。已核实全树无残留引用，
  Linux 的 `explicit_vdd` 分支（`ZAKO_DEVICE_ID`/`ZAKO_NAME`）完好保留。
- **验收**：Linux 客户端指定虚拟屏仍走 VDD 目标。

---

## 六、仍未完成（诚实清单）

1. **HLG 域分析源**：Linux 的分析器只按 PQ 解释像素，因此 HLG 会话没有 HDR Vivid、DV P8.4 也被门控
   拒绝。补齐需要新的分析源（预编码线性域，或 shader/readback 的 HLG 域映射），属独立特性；
   本机（PQ + NVIDIA）也无法端到端验证。
2. **niri / wlr-output-management 输出后端**（§2.10）：模式/HDR/拓扑在 niri 下仍走"合成器不可用"
   降级。实现需要 niri IPC 的真实 JSON 结构（本机未装 niri），拿到 `niri msg --json outputs` 与
   `niri msg --help` 的实测输出即可落地并配测试。
3. **D15 复制拓扑**（§1.4）：kscreen-doctor 无 replication 命令（已实测确认），KDE 上实现镜像需 KWin
   脚本或 libkscreen 后端，属独立特性。
4. **§四的有意保留差异**：如需翻转，按各条给出的理由逐项决策即可。
5. **DisplayID 扩展块（中，可选）**：> 655.35 MHz 的模式（3440x1440@120/144、3840x2160@90/120/144）
   目前只能明确拒绝。要真正支持，需要在 EDID 里追加 DisplayID 扩展块（24 位像素时钟），并让 base
   block 的首选 DTD 退回一个可行模式、由会话显式切到该模式；成败取决于内核 DRM 与 KWin 是否把
   DisplayID timing 纳入模式表，需实机验证。
6. **合成器未重探测 EDID 改写的可能**（低）：若实机出现"模式可行但始终未生效"，即为此因。

---

## 七、2026-10-22 上游同步新引入的缺口

同步点：`ed415026` → `7a0995e2`（33 个提交，403 文件，+29538/−12656）。本节只记录**本次同步带来
的新缺口**；连同 §一/§二 一起才构成完整待办。

### 7.1 🔶 管线帧率的"源更新"段在 Linux 恒为空（新）

- **现状**：上游把 `perf_recorder` 迁到 `src/streaming/perf_recorder.{cpp,h}`，新增
  `src/streaming/frame_rate_tracker.h`（`capture_source_kind_e { unavailable, dxgi_updates,
  wgc_arrivals, vdd_published }` 与 `capture_source_counter_t`）。平台钩子是
  `platf::display_t::frame_rate_source()`（`src/platform/common.h:685`，默认返回空指针）。
  只有 Windows 三个后端 override 它：`src/platform/windows/display.h`、
  `display_vdd.cpp:1453`、`display_wgc.cpp:447`。Linux 的 `src/platform/linux/kmsgrab.cpp:632`、
  `cuda.cpp:735`、`wlgrab.cpp:28` 都没有 override。
- **后果**：`GET /api/perf/current` 的三段（捕获交付 / 编码提交 / 编码输出）在 Linux 正常，只有
  "源更新帧率" 恒为 `unavailable`；`POST /api/perf/frame-rates`（`src/confighttp/runtime.cpp:463`）
  的按需订阅因此没有可订阅的源。
- **技术路径（已做可行性核查，结论：不能照搬 Windows 的两条路）**：
  - `wp_presentation` **不可用**：`wp_presentation.feedback()` 需要绑定到我们自己提交的
    `wl_surface`，而 wlr-screencopy 客户端不持有 surface，拿不到 `presented` 事件。协议 XML 虽然
    已 vendored（`third-party/wayland-protocols/stable/presentation-time/presentation-time.xml`，
    `GEN_WAYLAND` 一行就能生成代码），但语义上无法用来观测别人的输出。
  - `drmModePageFlip` 的 `sequence` **不可用**：page-flip 事件只发给 DRM master，而 kmsgrab 是非
    master 的抓帧客户端，收不到。
  - 因此 Linux 侧唯一可得的"源节奏"信号就是**合成器按需交付给 screencopy 的帧**，而那就是我们
    自己的捕获交付段——再记一遍会让"源 = 捕获"恒等，指标反而误导（比 `unavailable` 更糟）。
- **可选方案**：在 `src/platform/linux/vdd_edid.cpp` 那条链路上，若将来由我们驱动 VDD 的
  presentation（例如自建 wl_surface 或用 DRM master 的 headless 后端），才能真正量到
  `vdd_published`；当前架构下不做，`source_kind` 保持 `unavailable` 是诚实结果。
- **验收**（若未来做）：`/api/perf/current` 的源段非 `unavailable`，且 `docs/rtss-pipeline-fps.md`
  的口径在 Linux 上可复现。

### 7.2 ○ 依赖外网的单元测试（新）

- `DownloadFileTests/DownloadFileTest.Run/1` 访问
  `https://httpbin.org/redirect-to?url=/base64/aGVsbG8h`。离线机器必失败，与本仓代码无关。
- **处置**：联网环境跑，或在 CI 里跳过该参数组；不要为此改产品代码。

### 7.3 ○ 依赖系统已安装资产的单元测试（新）

- `tests/unit/test_crypto.cpp` 的
  `NvhttpApps.StreamsLargeAssetOverTlsAndPreservesNotFoundAndBadArguments` 断言"未知 appid →
  404"。但 `DEFAULT_APP_IMAGE_PATH` 是 `SUNSHINE_ASSETS_DIR "/box.png"`（`src/process.cpp:52`），
  编译期就定成本机安装前缀的绝对路径 `/usr/share/sunshine/box.png`；本机装过包，该文件存在 →
  实际返回 200 并把 box.png 流出。
- **已验证是环境假设、不是移植回归**：在用户命名空间里 `unshare -rm` 把空目录 bind-mount 到
  `/usr/share/sunshine` 后，该用例通过。
- **要常绿**：测试里把 `SUNSHINE_ASSETS_DIR` 指到临时目录，或把该用例标注成"需要无系统资产环境"。

### 7.4 已随本次同步自动继承（无需后续动作，记录以免误判为缺口）

- **HTTP 传输替换**：Simple-Web-Server → 自带 Beast 实现（`src/http/{types,server,client,
  tls_trust}.{h,cpp}`，`cmake/targets/beast_http.cmake` 建 `sunshine_http` 静态库）。本仓 24 个
  include 点、AUR PKGBUILD 的 `source=`/`prepare()`、`third-party/Simple-Web-Server` 子模块与
  工作区残留目录已全部清掉。`web::StatusCode` 的枚举名与旧 SimpleWeb 不同（`not_found` 等），
  本仓 Linux-only 文件已随合并改到位。
- **confighttp 拆分**：`src/confighttp.cpp` 4270 行 → 171 行，逻辑进 `src/confighttp/` 24 个文件；
  `saveVddSettings` 上移为 `display_device::vdd_settings::save()`（`src/display_device/vdd_settings.cpp`，
  已在 `cmake/sources/host.cmake:32`，Linux 也编）。
- **去门控重新打回**：`/api/vdd/status`（`src/confighttp/diagnostics.cpp:420-422`）与
  `/api/runtime/hdr` 的 `available`（`src/confighttp/runtime.cpp:121`）上游仍是 `#ifdef _WIN32`，
  本仓保留跨平台版本（冲突已手工解决）。
- **text_context 门控**：`notify_gui_alive` 扩到 6 参数（input_pane/uia/agent_present/observer_active/
  generation/agent_id）。Linux 无 GUI agent 上报，`gui_alive()` 恒 false，行为不变。
- **笔 barrel roll**：共享层（`src/pen_barrel_roll.h`、`pen_input_t::barrelRoll`）到位；inputtino 的
  `place_tool()` 没有 barrel roll 参数，Linux 平台端无处落地，`src/rtsp.cpp` 的
  `x-ss-general.penBarrelRoll` 广告也仍被 `#ifdef _WIN32` 挡住——客户端不会协商到，行为正确。
- **AMF 两个提交**（AMF 积压输出、AMD HEVC GOP）：`src/amf/*` 只在
  `cmake/compile_definitions/windows.cmake:118-137` 编译；新增的 `amd_hevc_gop_size` 配置键走共享层。
- **仓库根整理**：`assets/brand/`、`assets/installer/`、`scripts/vite/`、`docs/{readme,en,zh-CN}/`
  已就位；`cmake/packaging/linux.cmake` 自动指向 `assets/brand/sunshine.svg`，本仓额外安装的
  `logo-sunshine-*.png` 规则保留。
