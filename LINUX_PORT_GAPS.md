# Linux 移植遗留差距清单

- **基线**：`linux-migration` 分支，tag `v0.9.1-linux-full-cross-product`（pkgrel 38，2026-09-11）
- **对照**：fork Windows 版（AlkaidLab master `3d76d35d`）的全部增强功能
- **用途**：待办工作清单。每项含现状钩子（代码位置）、技术路径、预估工作量、验收标准
- **约定**：🔶 部分实现（有偏差）｜○ 未实现但可行｜⛔ 本质不可移植

---

## 总览

核心体验链路已全部对齐：虚拟显示器（原生 EDID 后端 + 配置全组合模式表 + 6 模式 prep + 独占还原 +
残留 adopt）、物理显示器 display_device（kscreen 分辨率/HDR/主屏/拓扑 + 持久化还原）、HDR 三件套
（静态透传 / HDR10+ 动态 / DV P8.1 RPU）、剪贴板文本双向、远程麦克风、ABR 前台检测、
托盘（状态实时化 + 消息框）/向导/打包。

剩余：**7 项部分实现（1.2 已解决，不再计入）、10 项可行未做、9 项本质不可移植**。另完成一轮
**已移植部分的 Windows 对齐审计（§五）**：修复 15 项偏差，其余按影响排序待决策。

---

## 一、🔶 部分实现（有已知偏差）

### 1.1 虚拟显示器 per-client 隔离

- **现状**：客户端切换时**销毁重建** VDD（`session.cpp` 的 `rebuild_old_vdd_id` 链路），
  单虚拟屏共享；Windows 是多实例并存，每客户端独立 GUID（`vdd_utils::generate_client_guid`）。
- **差距**：两个客户端无法同时各用各的虚拟屏。
- **若要做**：Linux 后端需要支持多个 EDID override 连接器并存（每个客户端占一个空闲物理
  连接器），`create_vdd_monitor` 带 GUID 参数化显示名。受限于机器上的空闲连接器数量。
- **工作量**：高。**验收**：两客户端同时各串各的虚拟屏互不干扰。

### 1.2 VDD EDID 模式表 —— ✅ 已完全解决（2026-09-11，tag `v0.9.1-linux-full-cross-product`，pkgrel 38）

**链式 CTA 扩展块**突破单个扩展块 6 模式的上限：块 1 承载数据块 + 首选时序副本 + 4 个附加模式，
后续 DTD-only 块各承载 6 个（`vdd_edid.cpp:256-275`，逐块校验和与 DTD 起始指针），
**配置分辨率 × 刷新率的全部可行组合**（`vdd_edid::mode_fits_pixel_clock_limit()` 像素时钟可行性
过滤）都能进 EDID，与 Windows SETMODES 的"全组合"语义等价；阶梯只剩一个防病态配置的 40 项保护
（`vdd_utils.cpp:1900-1904`）。Range Limits 描述符同步覆盖全部通告刷新率（此前写死 preferred±20，
与 144 Hz 模式自相矛盾）。

演进三步（详见报告 §十二进度 21）：

- `49800f6c`（v0.8.7）：阶梯从"每分辨率只配最高刷新率"改为可行组合叉积（首选分辨率保留全部配置
  刷新率），并修掉首选模式选到 3840x2160@144 后像素时钟溢出、被静默回绕成 71.38 Hz 的 bug。
- `4ad74c90`（v0.9.0）：链式 CTA 块——块 1 + 后续 DTD-only 块，模式空间实际不再受限。
- `fbb1b911`（v0.9.1）：非首选分辨率也携带全部配置刷新率。

**用户真实配置验证**（fps 60/90/120/144，720p…4K）：640 字节 / 4 扩展块 / 24 DTD / edid-decode
零告警——4K@60 首选（144/120/90 超像素时钟上限被可行性过滤）+ 3440x1440@90/60 + 其余分辨率
@144/120/90/60 全档。

**配套已完成项**：

- 手动创建参数化（`4e186f4b`/`2ce99874`，v0.8.3/v0.8.4）：偏好模式从 WebUI 可编辑的
  `resolutions`/`fps` 列表推导（面积最大分辨率 × 其最高可行刷新率），客户端会话模式仍优先
  （`cached_from_session`）；中途引入的 `vdd_manual_*` 键已删除，**无新增配置键**。
- 显示名对齐 Windows（`89cd047c`/`e3d11b37`，v0.8.5/v0.8.6）：EDID 名描述符用共享常量
  `ZAKO_NAME`（"Zako HDR"）；KDE 前缀的 "UQD" 是厂商 ID 字节的固有解码（该字节同时充当残留 VHD
  清扫/adopt 签名）。配置解析撤掉自创夹取，对齐 Windows `parse_vdd_resolution`/
  `parse_vdd_refresh_hz` 的宽松接受。
- 残留虚拟屏 adopt（`387a97fd`，v0.8.1）：EDID override 与强制连接器状态是 DRM 持久的，新进程
  首次查询时扫描 VHD 签名连接器、还原跟踪路径、从 DTD 解析回偏好模式。

**剩余小项**：生成器空白模型（8% / 2.5%）比 CVT-R 保守，个别真实模式（如 3440x1440@120）被
可行性过滤器排除——校准空白模型可再放宽；会话外 kscreen-doctor 热切已可用但未在 UI 暴露入口。
（EDID 参考向量已随派生 Range Limits 更新，见 §5.1 P15。）

### 1.3 HDR 亮度分析精度

- **现状**：CPU 亮度直方图，PQ 亮度域（`video.cpp` `analyze_pq_luma_frame`）。
- **差距**：Windows 是 D3D11 计算着色器逐像素 max(R,G,B)（scRGB）；亮度 ≤ maxRGB，
  饱和纯色场景 maxSCL 略偏低。消费端已按近似处理（99 分位当 maxSCL）。
- **若要做**：CUDA kernel（nvenc 会话有 CUDA hwdevice）或 Vulkan compute 对 YUV→RGB
  逐像素求 maxRGB。需要引入 CUDA 编译依赖（本机构建目前 `SUNSHINE_ENABLE_CUDA=OFF`）。
- **工作量**：高。**验收**：纯红/纯蓝测试图 maxSCL 接近真实值。实际观感收益存疑，优先级低。

### 1.4 剪贴板非文本类 + 非 KDE provider

- **现状**：`clipboard_host.cpp` 仅处理 KIND_TEXT（klipper D-Bus 只支持文本读写）；
  KIND_PNG / KIND_REF / KIND_FILE_OFFER 的**主机写入侧**缺失（客户端→主机方向的图片/
  大文件不会落到主机剪贴板；主机→客户端方向的图片无人采集）。非 KDE 桌面无任何 provider。
- **若要做**：
  - 图片：写 wlr-data-control Wayland 协议客户端（KWin 支持；wayland-scanner 生成协议码，
    构建已有 wayland 依赖）。SELECTION 源设 PNG+text/uri-list，写入用 MIME 技巧。
  - 大文件：主机侧也接 blob store（`clipboard_blob_store` 已跨平台），发 KIND_REF 帧。
  - 非 KDE：X11 XFixes selection 监听（项目已链接 X11）。
- **工作量**：中（wlr-data-control 客户端是大头）。**验收**：手机复制截图 → 主机粘贴为图片。

### 1.5 display_device 复制拓扑 + 非 KDE 桌面

- **现状**：`platform/linux/display_device.cpp` 拓扑每组单设备；`set_topology` 对多设备组
  直接拒绝。模式/HDR/主屏/拓扑读写全部依赖 kscreen-doctor（KDE Plasma）；其他桌面查询
  返回空 → 自动退化为历史 no-op 行为（不破坏，但无功能）。
- **若要做**：
  - 复制拓扑：kscreen-doctor 无 replication 设置命令 → 需走 KWin 脚本（同 ABR 前台检测的
    加载模式）或 libkscreen D-Bus。
  - GNOME：Mutter DisplayConfig D-Bus（`org.gnome.Mutter.DisplayConfig`，SetMonitorsConfig
    支持 mode/scale/primary），实现独立的 backend 类按 `XDG_CURRENT_DESKTOP` 分发。
- **工作量**：复制拓扑中；GNOME 后端中-高。**验收**：GNOME 会话下分辨率/HDR 切换可用。

### 1.6 帧管线契约（frame_contract）

- **现状**：策略层跨平台（`platform/frame_contract.cpp`），只有 Windows 采集端消费。
- **差距**：Linux 采集路径不消费契约（帧时间戳/管线追踪在 avcodec 路径已有
  `frame_timestamps`，但契约的"合成 HDR 前置滤波"等阶段无 Linux 对应物——多数依赖
  pre_encode_filter，见 ⛔ 表）。
- **结论**：随 pre_encode_filter 一起评估（⛔ 3 的伴生项），单独无意义。

### 1.7 AI API Key 加密存储

- **现状**：Linux 明文环境变量 `SUNSHINE_LLM_API_KEY`（`ai/credential_store.cpp`）；
  Windows 用 DPAPI。
- **若要做**：libsecret（gnome-keyring/kwallet 后端）——KDE 环境有 kwallet 的 secret
  service 兼容层。编译依赖 `libsecret`，可选发现（找不到回退环境变量）。
- **工作量**：低。**验收**：Key 存入 keyring，进程列表/env 里看不到明文。

### 1.8 音频设备位深匹配

- **现状**：虚拟 sink 固定 `PA_SAMPLE_FLOAT32`（`platform/linux/audio.cpp:81`）。
  Windows 用 PolicyConfig COM 按客户端位深建设备避免 WASAPI 重采样。
- **实际影响**：PipeWire 服务端自适应转换，质量无损；仅多一次转换的 CPU 开销（可忽略）。
- **结论**：**建议关闭**（不处理），除非实测发现问题。

---

## 二、○ 未实现但可行（按性价比排序）

### 2.1 HDR Vivid T.35 序列化器 —— ✅ 已完成（2026-09-11，`4595e349`）

avcodec 路径的 Vivid side data 本来就逐帧更新，但 FFmpeg 只有 CUVA 解析器、没有序列化器
（已用 bundled `libavutil.a` 符号核实：有 `av_dynamic_hdr_plus_to_t35`，无 Vivid 对应物）。
现改为在 `encode_avcodec()` 里用共享的 `serialize_vivid_t35()` 构建 T.35 载荷，按提交帧序号
暂存、在输出包按 pts 用 `hdr_bitstream::append_t35_unit()` + `insert()` 拼到首个 VCL NAL 之前，
与 DV RPU 共用一次 AVPacket 扩容；暂存队列有上限并带告警。

**Windows 不受影响**：Windows 的 avcodec 家族被显式标记为不能承载 Vivid
（`display_vram.cpp`），Vivid 走 NVENC/AMF 直连；因此该拼接在 `#if !defined(_WIN32)` 内编译，
Windows 会话标志恒为 false。**验收**：Vivid 客户端可解出逐帧元数据——回归测试覆盖
"序列化 → 包装 → 插到图像数据之前（prefix SEI type 39、CUVA T.35 头完整）"。

### 2.2 剪贴板图片 + 大文件（中）⭐ 推荐下一个

- 见 1.4 的路径。拆两步：先 wlr-data-control 图片双向（PNG），再 blob store 的 KIND_REF
  （大文本的 KIND_REF 回退同样适用，见 §5.2 C4）。
- **验收**：手机复制截图 → 主机 Ctrl+V 得到图片；主机复制图片 → 手机粘贴。

### 2.3 USB/IP 主机（中高）

- **现状**：`remote_usb/remote_usb_host_controller.cpp` 非 Windows 直接返回
  `usbip_host_backend::unsupported`。
- **路径**：Linux 是 usbip 的内核原生平台（`usbip-host` 内核模块 + `usbipd` 用户态工具）。
  把 unsupported 后端替换为：枚举可导出的 USB 设备（sysfs busid）→ `usbip bind` →
  TCP 3240 端口复用现有 reverse tunnel。fork 的协议层（`loopback_usbip_bridge`、
  `reverse_tunnel_service`）是跨平台源码。
- **前置**：内核 `usbip_host` 模块（`usbip` 包含用户态工具）。
- **验收**：客户端挂载主机 U 盘/手柄，读写正常。

### 2.4 GNOME Mutter 显示后端（中-高）

- 见 1.5。独立 backend 类 + `XDG_CURRENT_DESKTOP` 分发（`vdd_utils::hint_primary_output`
  已有分发骨架）。注意 `display_device.cpp` 当前所有读写函数都隐式假设 kscreen 存在。
- **验收**：GNOME Wayland 会话下客户端"优化游戏设置"能切分辨率/HDR 并还原。

### 2.5 DSU 运动数据服务器（低中）

- **现状**：`platform/windows/dsu_server.cpp`（DSU/cemuhook UDP 协议，向模拟器等第三方
  提供手柄运动数据）仅 Windows input.cpp 挂载。
- **路径**：inputtino 的 DS5 (uhid) 有加速度/陀螺仪数据源；照协议（UDP 26760）实现广播。
- **验收**：Cemu/模拟器识别到运动输入。

### 2.6 非 KDE 前台检测（低）—— niri 已覆盖（2026-09-11），其余待做

- **已完成**：niri（§2.11 / §5.5 R5）——轮询 `niri msg --json focused-window`，KWin 优先、niri 兜底。
- **仍缺**：X11：XFixes + `_NET_ACTIVE_WINDOW`/`_NET_WM_PID`（X11 库已链接）；
  wlroots 系（sway/Hyprland）：`ext-foreign-toplevel-list` / wlr-foreign-toplevel-management。
  两者都只需接入 `foreground_app.cpp` 的同一 producer 选择与缓存。
- **验收**：Hyprland/X11 会话下 ABR 日志出现前台切换。

### 2.7 AI Key libsecret 后端（低）

- 见 1.7。

### 2.8 DS5 本地触觉播放 sidecar（高）

- Windows `ds5/ds5_sidecar_client.cpp` + `mic_write` 的触觉 PCM 在主机直连手柄上播放。
- **路径**：`/dev/hidraw` 直写 DualSense 输出报告（udc3 打开 + 触觉报告格式），
  权限走 udev tag。基础 rumble 已由 inputtino 覆盖，这里缺的是 authored IR PCM 高保真回放。
- **验收**：直连 DS5 玩支持触觉的游戏，波形与 Windows 版一致。

### 2.9 NVENC SDK 直连（高，建议重估后可能降级/放弃）

- HDR10+/DV 已完成，avcodec 路径功能面已齐。直连的剩余增益：fork 的细粒度码控、
  lookahed、多硬件实例。若 2.1 Vivid 也完成，T.35 手写序列化器反而是直连的*负资产*
  （avcodec 不需要它）。
- **建议**：搁置，等实际画质/延迟瓶颈出现再评估。

### 2.10 Tauri 控制面板选择性移植（高，收益低）

- 面板约 20 个系统模块仅 Windows 实现（vdd/vigem/vmouse/rtss/hwinfo(WMI)/elevation/
  注册表自启/dualsense elevated/shell 右键菜单等）。WebUI 已覆盖大部分。
- **若有需求**，只值得挑：QR 配对、实时监控（hwinfo/RTSS → Linux 对应物是
  `nvidia-smi`/`amdgpu_top`/`_PIPEWIRE` 统计）、文件右键共享（KDE ServiceMenu 更自然）。
- **建议**：搁置；文件右键共享若想要，走 KDE ServiceMenu（低工作量）单独立项。

### 2.11 niri 支持（中）— 第二目标环境（2026-09-11 立项）

- **背景**：目标是 **KDE + niri** 双环境、通用方案优先，而原实现基本是 KDE 专属：
  - **物理显示器 display_device**：全部经 kscreen-doctor → niri 下 `query_outputs()` 为空，
    `apply_config` 走"合成器不可用"分支静默成功（§5.2 D18），客户端的模式/HDR 请求被忽略。
  - **VDD**：DRM 层（EDID override、强制连接、pidfd 借 master 指派 CRTC）与合成器无关，且 niri
    默认自动启用新输出，虚拟屏本身可用；但 `kscreen-doctor output.X.enable` 与主屏 hint 在 niri
    下无效（仅日志噪音）。
  - **ABR 前台检测**：KWin 脚本在 niri 下不可用 → 降级为空结果（§5.2 F4）。
- **已完成（2026-09-11，第三轮）**：
  1. **前台检测 producer —— ✅**（`src/platform/linux/foreground_app.cpp`）：新增 niri 路径，
     轮询 `niri msg --json focused-window`（2s，`run_logged` 有界执行），JSON 逐字段、带类型检查
     地映射到 `info_t`（title/app_id/pid；`null` = 无焦点窗口，不更新缓存，与 KWin/Windows 语义
     一致）；后端选择在启动时决定——**先探测 KWin（`org.kde.KWin` 的 NameHasOwner），KDE 保持
     原路径**，无 KWin 且 niri 查询可用才启用 niri；解析器有 4 个单元测试（正常/`null`/字段缺失
     或类型错误/畸形 JSON）。
  2. **VDD 输出的合成器无关启用 —— ✅**：新增 `enable_output_via_compositor()`，
     KDE → kscreen-doctor（行为不变）、niri → `niri msg output <name> on`、其他 Wayland →
     `wlr-randr --output <name> --on`（wlr-output-management，覆盖 sway/Hyprland/river…）、
     X11 → `xrandr --output <name> --auto`；都没有则只记一次 info 并依赖合成器自动启用。
     `hint_primary_output` 对 niri 明确记录"niri 无主屏概念"。DRM 层的 CRTC 指派仍是真正点亮
     输出的手段，这些命令只是合成器侧的推力，失败不影响现状。
- **待做**：
  1. **display_device 的 niri / 通用输出后端（中）**：`query_outputs()`/模式应用/HDR 目前只有
     kscreen 实现。niri 需用其 IPC（`niri msg outputs` / `output <name> mode|on|off|scale|position`），
     通用兜底用 wlr-output-management（`wlr-randr`，覆盖 sway/Hyprland 等）。注意 HDR 是否有
     IPC/配置项需在 niri 上确认，无则报 unknown 而不是猜。
  2. **前台检测的 wlroots/X11 兜底（低-中）**：`ext-foreign-toplevel` / X11 XFixes，接入同一
     producer 选择。
- **验收**：niri 会话下 ABR 前台 exe 正确切换；客户端分辨率请求能作用到目标输出；VDD 创建无
  kscreen 报错噪音。**注意**：本机未装 niri，命令语法必须在 niri 环境实测；一律先做能力探测，
  探测失败必须保持现有降级路径（不得影响 KDE 与非 KDE 现状）。

---

## 三、⛔ 本质不可移植（含原因与 Linux 对应物）

| Windows 功能 | 不可移植原因 | Linux 对应物 / 状态 |
|---|---|---|
| Zako Direct 零拷贝借帧 | D3D11 共享纹理，无 Linux 对等机制 | KMS 捕获直读 DRM plane（功能面已覆盖） |
| WGC 捕获 | Windows Graphics Capture API | kmsgrab ✅ |
| rtx_hdr / TrueHDR 链 | RTX Video HDR = NVIDIA Windows 驱动 API + D3D11 合成管线 | ⛔ 无；且 rtsp.cpp 的 TrueHDR PQ 链路整体 Windows 门控 |
| AMF 硬件编码 | AMD Windows 运行时 | VAAPI（上游已有；N 卡不适用） |
| HLG 捕获源 → DV P8.4 | 内核 `HDR_OUTPUT_METADATA` UAPI 只定义 PQ/SDR | ⛔ 等内核演进；P8.1 已覆盖 DV 主流 |
| ZakoVDD 硬件光标导出通道 | Windows 驱动私有通道 | KMS 光标平面（合成器渲染光标进捕获）✅ |
| 触摸键盘自动唤起 | Windows 任务栏注册表机制 | 头文件本身非 Windows 即 no-op，无需处理 |
| nvprefs 驱动设置 | NVIDIA Windows 控制面板 API | nvidia-drm modeset 参数（系统层已配置）✅ |
| 服务包装器 / UAC 提权 / Win 深色模式 / GFE 兼容 | Windows 平台概念 | systemd 用户服务 ✅ / 无此概念 ✅ |

---

## 四、附带说明（审计时确认的 Windows 行为差异，非缺口）

- avcodec 路径（Windows 上的 QSV/软编）现在也会跑 CPU 亮度分析器 + DV P8.1 注入
  （与 NVENC 直连同门控）——fork 意图内的能力补齐，每帧 ~1-2ms CPU。
- `main.cpp` SIGINT/SIGTERM 处理器无条件调 `end_tray()`——Windows 控制台停止时托盘图标
  提前移除（幂等、线程安全）。
- `main.cpp` 信号处理器、`vdd_capability` 的"真实版本上报"在 Windows 上均为行为恒等
  或良性变化（审计详见 2026-09-10 提交 `3756caf6` 前后的分支审计记录）。
- 托盘 VDD 子菜单项新增 `checkbox=1`（Qt 渲染勾选态所需）——Windows 托盘仅凭 `checked` 画
  MFS_CHECKED，该字段在 Windows 上被忽略，行为恒等；VDD 菜单消息框（`QMessageBox`）整体在
  `#ifndef _WIN32` 内，Windows 仍走 `MessageBoxW`。
- 虚拟屏 EDID 名由 "Foundation VDD" 改为共享常量 `ZAKO_NAME`（"Zako HDR"）——用户可见命名对齐
  Windows（KDE 显示的 "UQD" 前缀是厂商 ID 字节的固有解码，非命名分歧）。
- **约定沉淀**：`AGENTS.md` 新增 Windows parity 三条（共享头文件提供规范常量、移植语义含边界与
  默认值、用户可见命名与 Windows 一致），后续移植按此自查；报告侧证据见
  `LINUX_MIGRATION_REPORT.md` §十二进度 20。
- **测试基线（2026-09-11，pkgrel 38 构建树复核）**：`ctest` 13 套件 12 通过，聚合套件
  `test_sunshine` 仅 AudioTest / MouseHIDTest / EncoderTest 的 `SetUpTestSuite` 失败
  （0 个断言失败，需真实音频/输入/编码器环境），与既有基线一致。

---

## 五、Windows 对齐审计（2026-09-11）

对**已移植部分**做了逐行 Windows 对照审计（VDD / display_device / 剪贴板 / 麦克风 / ABR / 托盘 /
HDR 编码七块），静态源码对照，未做 Windows 侧运行验证。过程提交：`f0f13e60`（VDD）、
`b5e83d0e`（display_device）、`45e09e6a`（HDR 分析）、`89a588d6`（剪贴板/麦克风/ABR/托盘）。

### 5.1 本轮已修复

| # | 差距（审计证据） | 处置 |
|---|---|---|
| P1 | VDD 模式表解析自创语义：不 trim、不查尾随字符、小数刷新率被整型截断（59.94→59） | 8 个 helper 移入共享段（与 Windows 逐字一致）；Linux 阶梯改用 `prepare_vdd_settings()`（原为返回 `{}` 的桩），会话模式并入列表 |
| P2 | per-client 尺寸类表在 Linux 重复硬编码（违反 AGENTS.md 规范常量约定） | 收敛为共享 `client_physical_size_for_class()`，两平台共用 |
| P3 | `create_vdd_monitor` 丢弃客户端物理尺寸与 HDR 亮度，EDID 从未个性化 | 写入 EDID（cm→mm；max/min/maxFALL nits；非正值与 <0.02 nit 最小值保留参考默认） |
| P4 | 剪贴板 SDP 在 Linux 通告 `clipboard_image`，而唯一 provider 只支持文本 | 仅 Windows 通告 image 位 |
| P5 | 单口味文本帧 token 非零（GUI agent 契约：单口味 = 0，非零表示可合并突发） | 文本帧固定 token 0 |
| P6 | `microphone_redirect_backend=disabled` 被忽略 | 生效（返回 -1，与 Windows 同） |
| P7 | 麦克风写失败一律 -2 且释放设备 | 区分 -2（连接终止/被杀，释放）/ 0（超大帧，等同 `AUDCLNT_E_BUFFER_TOO_LARGE`）/ -1（其他，不释放） |
| P8 | KWin 缺 pid 时 `foreground_exe` 冻结、`app_changed` 不触发 | 同类别保留上次 pid；共享消费端在 exe 变化且 pid=0 时也判定切换（Windows 中性） |
| P9 | 托盘确认框默认按钮 No（Windows `MB_YESNO` 默认 Yes）；reset 提示的 warning 图标被忽略 | 默认 Yes；yes/no 框也按 `as_warning` 设图标 |
| P10 | 模式匹配容差 0.051 Hz（Windows 1 Hz 模糊比较 + 取最近模式） | 1 Hz + 最近候选（59.94 面板 + 60 fps 不再配置失败） |
| P11 | HDR 读回 enabled 即跳过（Windows 仅 disabled==disabled 跳过，使能总重发） | 对齐 Windows 条件 |
| P12 | 空 device_id（Display: Auto）未解析为主屏 → 模式/HDR 作用于**所有**输出，`ensure_only_display` 空转 | `find_one_of_the_available_devices("")` 解析为 kscreen priority 1 的输出 |
| P13 | 亮度分析器对平面 10-bit 用 P010 的 `>>6` 解包：YUV444P10LE 读成全黑却标记 valid；YUV420P10LE 被直接拒绝 | 平面格式改掩码 `&0x3FF`，接受 YUV420P10LE |
| P14 | avcodec 路径仍打"DV 协商但无 RPU"的过时告警 | 删除（P8.1 RPU 已注入，门控在 session configure 内自报） |
| P15 | VDD EDID 参考向量钉在旧 Range Limits（40–80 Hz） | 更新为派生值（55–65 Hz），并注明与 sunshineVD `generator.py` 的有意偏差 |

### 5.2 待决策 / 未修（按影响排序）

**高 —— 已清零**（A1 麦克风背压 §5.7、F1 分析覆盖面 §5.8、F3 Vivid §2.1 均已落地；余下为中/低）

**中**

- **F4 HLG 无分析源、P8.4 门控被拒**（= §2.1 的伴生项，需先有 HLG 域分析）。
- **D7 "blank HDR toggle" 有意不移植**：Windows 在切换 HDR 前把新启用显示器先切到相反状态、等
  2333 ms 再设最终值，是 Windows 显示栈（IDD/VDD）的"颜色发白"清理手段。Linux 的 VDD 是真实
  DRM 连接器、无对应症状，移植只会给每次新启用显示器加 2.3 s 延迟和一次多余 HDR 翻转；如实测
  出现同样症状再补（§5.6 已记录该判断）。
- **D15/D16 复制拓扑不可表示 + 校验边界不一致**：KDE 镜像会话读回为多个扩展屏，请求复制组被拒绝
  （= §1.5）；`is_topology_valid` 比自身 setter 宽松。
- **D18 compositor 不可用时静默返回成功**（= §1.5）：非 KDE/旧合成器下客户端的模式/HDR 请求被忽略。

**低**

- **C5 剪贴板线协议常量跨语言同步**：C++ 侧已收敛到 `clipboard_bridge.h`（版本/kind/帧头/内联阈值/
  TTL），但 Rust agent（`clipboard.rs`）仍是独立副本，值变动需人工同步；代码生成机制待决策。
- **A5 麦克风契约边界**：null samples → 0（Windows -1）、重复 init → 0（Windows -1）、返回字节数
  2 B/帧（Windows 端点相关 4 B）、缓冲属性服务端默认（Windows 显式 100 ms）。
- **A6 虚拟麦克风命名** `Sunshine-Virtual-Microphone` 为 Linux 自创（Windows 是驱动提供的
  "VB-Audio Virtual Cable"），无共享常量。
- **F3' 前台 exe 语义**：Linux 报 Wayland app class，Windows 报进程映像名（含 `.exe`），ABR 提示词
  按 `.exe` 措辞。
- **D11 空容器契约相反**：Linux 空 device_ids = 全部输出、空 map 返回 true；Windows 分别返回 `{}`/false
  （Linux 的还原路径以"空 map = 无需还原"为由保留 true，见 §5.6）。
- **D17 friendly name 仅为连接器名**（如 `DP-1`），`get_display_name` 直通、非 VDD 的 friendly-name
  查找不可用；WebUI 设备列表显示连接器名且 HDR 状态恒 unknown。
- **D19 日志文案差异**：同一事件 Linux 英文 / Windows 中文（"串流结束"等）。
- **F11 WebUI HDR 状态端点硬编码 `available=false`**：Linux 无生产者上报（即使分析器在本机可用）。
- **F9 直方图估计器差异**：Linux 精确 1024 码直方图 vs Windows 256 bin 单元采样——语义一致、数值不同，
  不建议改（信息性）。

### 5.3 审计确认无差异（抽样）

- 剪贴板：帧布局/版本与 kind 常量/短帧丢弃、60 KB 内联阈值、5 s 回声 TTL、30 s `gui_alive` 窗口、
  会话起始基线行为。
- 麦克风：mono 48 kHz S16LE、960 帧（20 ms）块、无部分写、返回码形状（0/-1/-2 语义）。
- 前台：`info_t` 字段形状与默认值、消费端 10 s 限流、缓存加锁返回副本。
- 托盘：菜单索引/文案/i18n key/回调、VDD 子菜单结构与共享 `ZAKO_NAME`、重启路径，
  `checkbox` 字段在 Windows 被忽略（行为恒等）。
- display_device：持久化文件名与 JSON schema（跨平台可互读）、apply/revert 编排与 fail_guard、
  VDD destroy 决策、主屏检测（priority 1 ≡ 位置 (0,0)）、HDR 通用标志、SDR 会话还原、
  `unknown` 状态跳过。
- HDR：ST2084/PQ 常量全树唯一来源（无 `2399/4096×32`、无 m2×32 的再推导，HLSL 副本数值一致）；
  统计字段完整；10-bit 限幅重映射；HDR10+ 字段推导；EMA/场景切换/32 帧 Vivid 窗口等时间策略共享；
  DV L1 与 RPU 注入在三条编码路径共用同一实现。

### 5.4 第二轮修复（2026-09-11，KDE + niri 目标立项后）

| # | 项 | 处置 |
|---|---|---|
| R1 | **HDR Vivid T.35 在 avcodec 路径缺失**（= §2.1，原"推荐下一项"） | `encode_avcodec()` 内构建 CUVA T.35、按 pts 拼接（与 DV RPU 共用一次 AVPacket 扩容），仅在 Linux 编译；新增回归测试 |
| R2 | **麦克风未切换默认录音设备**（A3） | 复用文件内 pa_context helper：记住当前默认源 → 指向 null-sink monitor → 释放时还原；对 PulseAudio/PipeWire 通用 |
| R3 | **规范标识符仍被重新硬编码**（AGENTS.md 约定） | `ZAKO_DEVICE_ID`（原先 Linux 分支裸写 `"23172"`）入 globals；剪贴板线协议常量入 `clipboard_bridge.h`，`clipboard_host.cpp` 全面改用 |
| R4 | **目标环境只按 KDE 实现** | `AGENTS.md` 写明 KDE + niri 双目标与"通用兜底优先、后端必须探测并优雅降级"；niri 具体差距立项为 §2.11 |

**测试基线**：本轮结束仍为 12/13 套件通过、聚合套件 0 断言失败（新增 1 个位流回归用例，
`HdrBitstream` 共 21 个用例）。

### 5.5 第三轮修复（niri 起步，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R5 | **niri 会话下 ABR 前台检测缺失**（§2.11 / F4） | `foreground_app` 新增 niri producer：轮询 `niri msg --json focused-window`，JSON 逐字段类型检查映射；后端选择"KWin 优先、niri 兜底"（探测 `org.kde.KWin` 的 NameHasOwner）；解析器 4 个单元测试 |
| R6 | **VDD 输出启用与主屏 hint 硬绑 kscreen**（§2.11） | 新增 `enable_output_via_compositor()`：KDE 保持 kscreen-doctor，niri → `niri msg output <name> on`，其他 Wayland → `wlr-randr`，X11 → `xrandr`；`hint_primary_output` 对 niri 记录其无主屏概念；DRM CRTC 指派仍是主路径，命令失败不影响现状 |

**测试基线**：12/13 套件通过、聚合套件 0 断言失败（新增 `ForegroundApp` 4 例）。

**下一轮**：§2.11 待做第 1 项（display_device 的 niri / wlr-output-management 输出后端）、
剪贴板图片与大文件（§2.2）、display_device 失败处理补齐（§5.2 D5/D7/D8/D9/D13/D14）。

### 5.6 第四轮修复（display_device 失败处理，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R7 | **D5 模式无严格重试、部分失败不回滚** | `set_display_modes()` 重写为 Windows 的三段式：先按 1 Hz 模糊+最近模式应用 → 校验全部匹配 → 不匹配则以**精确刷新率**重试一次（对应去掉 `SDC_ALLOW_CHANGES`，让用户自定义模式也能选中）→ 仍失败则把进入时的快照 `original_modes` 整体回滚并返回 false |
| R8 | **D9 HDR 部分失败不回滚** | `set_hdr_states()` 先快照 `original_states`，任一设备失败即回滚到快照并返回 false（Windows `device_hdr_states.cpp` 同构） |
| R9 | **D10 未知 HDR 状态应快速失败** | 请求的设备在读回里没有 HDR 信息时，立即以明确日志返回 false，不再对注定失败的命令重试 3 次 |
| R10 | **D14 拓扑 set 未校验** | `set_topology()` 拆出 `apply()`，应用后经 `wait_for_topology(3 s)` 收敛再复核；确实不一致则回滚到进入时的拓扑并返回 false（合成器完全无响应时保持历史容忍，不误判） |
| R11 | **D8 HDR 前无稳定性等待** | 新增 `wait_for_display_stability()`（10 × 500 ms，上限 5 s）：本调用改过拓扑/模式时，等到目标设备都有可读的模式与 HDR 状态再切 HDR；超时只告警继续（与 Windows 一致），同时避免 R9 的快速失败被"读回滞后"误触发 |
| — | **D13 还原顺序 + 还原后 HDR 复核** | **经复核 Linux 已由构造覆盖**：还原是"先恢复初始拓扑、再按当前已启用设备过滤后还原 HDR/模式"，被重新启用的显示器天然包含在过滤后的集合里，等价于 Windows 的"还原后再修 HDR"，无需改顺序（改动反而会动到用户已实测的还原路径） |
| — | **D7 blank HDR toggle** | 有意不移植，理由见 §5.2 |

**测试基线**：12/13 套件通过、聚合套件 0 断言失败。这些路径需要真实合成器才能端到端验证
（本机 agent 会话无 WAYLAND_DISPLAY/bus），逻辑对照 Windows 实现逐条核对。

### 5.7 第五轮修复（麦克风背压，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R12 | **A1 麦克风背压契约在 Linux 无生产者** | 新增 `src/platform/linux/mic_queue.h`（有界帧队列：容量 5 帧 = 100 ms，与 Windows 端点缓冲同量级）+ `audio.cpp` 内的 writer 线程：`write_mic_pcm()` 只入队（**永不阻塞共享混音线程**），队列满即返回文档约定的 `0`（丢帧，等价 Windows 的 padding 预检），`stream.cpp` 的 `wasapi_backpressure_drops` 因此变为真实计数；`pa_simple_write` 的阻塞写与错误码映射（-2 设备丢失 / -1 一般错误）移到 writer 线程，错误在下一次 `write_mic_pcm()` 上报，调用方的重初始化契约不变 |
| — | 7 个队列单元测试 | FIFO 顺序与载荷、满队列报背压、空帧不算背压、stop 唤醒空队列等待者、stop 丢弃待发帧、pop 被 push 唤醒、reset 重新武装（全部有界，不会挂住测试） |

**说明**：`release_mic_redirect_device()` 与析构都会先 stop+join writer 再销毁 `pa_simple`
（顺序受控）；若 PipeWire/PulseAudio 服务端彻底僵死，`join` 仍可能被阻塞中的写卡住——这与改造前
"写调用直接卡在会话线程上"的暴露面相同，未加重也未消除（无 timeout 版 pa_simple API 可用）。

**测试基线**：12/13 套件通过、聚合套件 501 用例 489 通过 / 12 跳过 / 0 断言失败。

### 5.8 第六轮修复（HDR 分析覆盖面，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R13 | **F1 亮度分析器只挂在 avcodec 软件设备** | 新增"采样下载"生产者：Linux 的硬件编码设备（VAAPI/CUDA，`data != nullptr` 且自报无分析能力）在 PQ + 请求了 HDR10+/DV P8.1 且 `hdr_luminance_analysis != off` 时，session 里用 `av_hwframe_transfer_data()` **每 4 帧下载一帧**到缓存的软件帧，复用同一个 CPU 分析器（Windows 的采样间隔也是 1/4）；下载格式不支持 10-bit 或传输失败时**关闭本会话的分析**并告警（退化到"无动态元数据"，不影响串流）。同时把分析器的格式判定抽成 `luminance_analysis_format_supported()` / `..._is_msb_aligned()`，避免新代码重复格式清单 |

**Windows 中性**：整段能力判定与采样都在 `#if !defined(_WIN32)` 内；Windows 的采集设备自己产出统计并
自报能力（`display_vram.cpp`），标志恒为 false，路径不会触发。Linux 软件设备路径（`data == nullptr`）
也不受影响——它本来就在 `convert()` 里分析，且 `hdr_luminance_analysis_available` 为真。

**残留与代价（如实记录）**：

- 该路径**未在 VAAPI/CUDA 硬件上实测**（本机 NVIDIA 且按文档 `SUNSHINE_ENABLE_CUDA=OFF`，走的是软件
  设备路径）——已按"失败即降级"设计，最坏情况回到改动前的行为。
- 每 4 帧一次**全分辨率**显存→内存下载（1080p60 P010 约 6 MB/帧、约 90 MB/s），比 Windows 的 GPU
  端分析 + 小缓冲回读重；如后续在 AMD/Intel 上觉得开销明显，可加采样降分辨率（swscale）或改用
  shader 分析。
- 软件设备路径仍是**每帧**分析（Windows 为 1/4 帧、≤1080p），因此两侧的时间滤波推进节奏不同
  （§5.2 F7，仍开着）。

### 5.9 第七轮修复（枚举语义与剪贴板写入，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R14 | **D12 枚举 `active` = "已连接"而非"已启用"** | `enum_available_devices()` 现在读 DRM sysfs 的 `enabled`（CRTC 已绑定的状态，语义等同 Windows 的 `DISPLAYCONFIG_PATH_ACTIVE`）：连上但被桌面禁用的显示器报 **inactive**，VDD 保活/断电列表不再把它重新点亮。属性不可读（老内核）时回退到原来的"已连接即 active"；**虚拟屏例外**——它的通路由本后端（pidfd 借 DRM master + CRTC 指派）管理，不由桌面决定，仍按"live 即 active"处理（`live_virtual_display_connector()` 在循环外解析一次，避免 `is_vdd_connector()` 递归枚举）。新增 2 个单元测试：sysfs 非 enabled 的连接器不得报 active；非 enabled 的连接器必须仍被枚举到（inactive） |
| R15 | **C3 klipper 写操作在 enet 控制线程同步阻塞** | 客户端→主机的剪贴板写入改为**入队**（上限 8 条，溢出丢最旧并告警），由 provider 自己的 poll 线程在两次读取之间用它的 bus 执行 `klipper_set`；poll 线程的 1 s 等待改为条件变量 `wait_for`（有写入立即唤醒），`stop()` 也会唤醒它。enet 控制线程不再做同步 D-Bus 调用，klipper 僵死不会阻塞控制包处理；回声抑制仍在**入队时**记录（保持原有语义），写入成功后同步 `last_seen` 以免把自己写的内容当成主机侧变更回发 |

**测试基线**：12/13 套件通过、聚合套件 503 用例 491 通过 / 12 跳过 / 0 断言失败。

### 5.10 第八轮修复（剪贴板大文本 blob 回退，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R16 | **C4 60 KB 以上文本无 blob 回退（Linux 直接丢弃）** | 主机→客户端：超过 `kInlineThresholdBytes` 的文本改为 `clipboard_blob_store::put()` 后发 `kKindRef` 描述符帧（与 GUI agent 同路线，对端从本机 blob HTTP 端点取字节）；客户端→主机：新增 `kKindRef` 入站处理——按 agent 语义解析 `{"id","mime","size"}`、校验 id（非空且 ≤128）、只接受 `text/*`（图片/文件投递仍归 GUI agent）、从本地 blob store 取回后走既有回声抑制与 klipper 写入队列 |
| R17 | **线协议编解码散落在 provider 内、无法测试** | 新增 `src/clipboard_wire.h`：帧编解码（`encode`/`encode_text`/`encode_ref`/`parse_header`/`payload_of`）与 REF 描述符解析（`parse_ref_descriptor`），语义逐条对照 Rust agent 的 `encode_frame`/`decode_frame`/`RefMeta`（含未知版本/未知 kind/负载截断/超长 id 的拒绝）。MIME 常量（`kMimeText`/`kMimePng`）与 id 上限进 `clipboard_bridge.h` 共享；新增 6 个**跨平台**单元测试（Windows CI 同样会跑） |

**仍未覆盖**：图片类（KIND_PNG）双向与文件投递（KIND_FILE_OFFER）在 Linux 主机侧仍不支持（= §1.4/§2.2）。

**测试基线**：12/13 套件通过、聚合套件 509 用例 497 通过 / 12 跳过 / 0 断言失败。

### 5.11 第九轮修复（剪贴板回声与事件驱动，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R18 | **C6 回声抑制单槽 vs agent 的 16 项环形** | 新增 `src/clipboard_echo.h`：16 项 `(kind, payload-hash)` 环形 + TTL（默认取共享 `kEchoTtl`），语义逐条对照 Rust agent 的 `EchoState`（record 满则丢最旧、is_echo 先剪枝再按 kind+hash 比对；用 kind 字段合并 agent 的文本/图片两条环）。provider 的 echo 状态换成该环形并加锁使用；「连续两次客户端写入后主机复制旧值会被多广播一次」的问题消失。5 个跨平台单元测试（匹配/多写入/过期/容量淘汰与环绕/clear） |
| R19 | **C7 1 秒轮询 vs klipper 变更信号** | 订阅 `org.kde.klipper.klipper` 的 `clipboardHistoryUpdated` 信号（`sd_bus_add_match`），每轮先 `sd_bus_process` 排空总线；等待从 1 s 改为 200 ms（有排队写入立即唤醒），读到变更信号即刻读取剪贴板，**1 s 周期读仍作兜底**（信号不可用时仅记 debug 并退回原行为）。已用独立小程序在**真实会话总线**上验证：匹配规则被总线接受、`sd_bus_get_fd`/`sd_bus_process` 行为符合循环预期（klipper 正在运行） |

**测试基线**：12/13 套件通过、聚合套件 514 用例 502 通过 / 12 跳过 / 0 断言失败。

### 5.12 第十轮修复（托盘高级设置与文案，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R20 | **T1 托盘缺 Advanced Settings 子菜单（Linux）** | 子菜单在 Linux 打开，且**两平台布局完全一致**（导入配置 / 导出配置 / 重置为默认 / 分隔线 / 清理缓存 / 重置显示器）——顺带把 `update_menu_texts()` 的平台分叉合并成一份索引表，避免再出现索引漂移。三个原本是 Linux 桩的功能按 Windows 语义实现：**导入**（Qt 文件对话框 → `is_safe_config_path`/`is_safe_config_content` 校验 → 备份 `.backup` → 写 `.tmp` 后 `rename` 原子替换 → 询问是否重启应用）；**导出**（保存对话框 → 仅允许常规 `.conf`、拒绝符号链接 → 读当前配置 → `.tmp`+`rename` → 成功提示）；**重置为默认**（确认框 → 备份 → 清空配置文件 → 成功/失败提示）。原本不可达的两处 Linux 实现（重置显示器、清理缓存）现在都被菜单引用，且清理缓存补齐了 Windows 同款确认框 |
| R21 | **T4 退出确认文案提到不存在的 GUI** | 新增 Linux 专用键 `KEY_QUIT_MESSAGE_NO_GUI`（EN/ZH/JA 三语，去掉"(This will also close the Sunshine GUI application.)"一句），Windows 仍用原键与原文案。另新增 `KEY_IMPORT_SUCCESS_MSG`、`KEY_EXPORT_ERROR_PATH` 两个键（同样三语），供上面的对话框使用 |

**Windows 影响**：菜单数组与索引在两平台本就相同，Windows 的初始化语句、回调与文案**逐字未变**；
新增的 i18n 键 Windows 不使用。**测试基线**：12/13 套件通过、聚合套件 514 用例 502 通过 / 12 跳过 /
0 断言失败（本轮无新增用例；托盘交互需图形会话，未做自动化）。

### 5.13 第十一轮修复（占位 HDR10+ 元数据与前台缓存失效，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R22 | **F5 无统计时仍发占位 HDR10+ SEI** | avcodec 路径不再在会话建立时预挂 HDR10+ side data（那会把"满亮度"的占位值当成真实测量发出去）。改为**懒挂载**：`update_hdr_dynamic_metadata()` 在**第一帧拿到有效统计**时才 `av_dynamic_hdr_plus_create_side_data()` 并用 `init_hdr10plus_side_data()` 初始化窗口/椭圆/色调映射等静态字段，逐帧值仍由真实统计填充；会话上新增 `hdr10plus_side_data_wanted`（= 该 codec/传输函数能承载 **且** 分析器可用）。这样"完全没有分析器的会话"与"首帧尚无统计的头几帧"都不再发伪造元数据，与 Windows 原生路径"只有 valid 统计才构造元数据"一致 |
| R23 | **F2 前台缓存无失效机制** | 新增 `clear_cache()`，在**生产者已知死亡**时清空缓存：KWin 脚本静默后重载失败（`install_kwin_script` 返回 false）即清空；niri 查询失败（niri 重启/socket 消失）同样清空。ABR 因此退回"基于启动器"的分类，而不是继续按已关闭的游戏调整码率；长时间不变的前台窗口仍不会因为"看起来陈旧"被误清（这是有意保留的语义） |

**Windows 影响评估（R22，共享代码）**：默认配置（分析开启）下唯一差别是**首个有效统计到达前的几帧**
不再携带伪造 SEI；显式关闭分析（`hdr_luminance_analysis=off`）时 Windows 的 ffmpeg 路径也不再发占位
SEI——这与该平台原生路径（无有效统计就不构造元数据）一致，因此判定为修正而非破坏。R23 是
Linux-only 文件（`src/platform/linux/foreground_app.cpp`），Windows 不涉及。

**测试基线**：12/13 套件通过、聚合套件 514 用例 502 通过 / 12 跳过 / 0 断言失败（本轮改动位于
会话/编码路径，需真实编码器与合成器才能端到端验证）。

### 5.14 第十二轮修复（分析节奏与上限，2026-09-11）

| # | 项 | 处置 |
|---|---|---|
| R24 | **F7 分析节奏与 Windows 不一致** | 共享头新增 `hdr_analysis_interval = 4`（以及 Windows 采样上限 `hdr_analysis_max_width/height`），Windows 的 `display_vram.cpp` 与 Linux 的 CPU 分析器、硬件下载路径**统一引用同一常量**。CPU 路径因此从"每帧全分辨率"改为**每 4 帧一次**：共享时间滤波以 `sample_sequence` 识别新样本（`video_hdr_metadata.h` 明确写着"分析运行在帧率之下，只有新 sequence 才推进状态"），所以此前 Linux 的 EMA/场景检测/Vivid 启动门按帧推进得比 Windows 快 4 倍，现在两侧一致；帧间沿用上一次统计（这些滤波本就会忽略重复样本）。CPU 直方图开销随之降到 1/4 |
| R25 | **F8 `analysis_max_nits` 硬编码** | 改为引用新的共享常量 `st2084_peak_nits`（PQ 的分析上限），并注明 HLG 落地时须按 Windows 规则取 `min(display peak, 该上限)`；数值不变，消除魔法数字 |

**残留（如实记录）**：Linux CPU 路径仍读**整帧**（Windows 会在 GPU 端缩到 ≤1080p 再回读），
所以 4K 下采样像素量仍约为 Windows 的 4 倍——CPU 端的缩放开销与直方图本身相当，故未加缩放；
节奏对齐后开销已降为原来的 1/4。另外 Windows 的分析是**异步且陈旧一帧以上**，Linux 是同步当帧，
这一差异保留（Linux 的元数据更"新"）。

**测试基线**：12/13 套件通过、聚合套件 514 用例 502 通过 / 12 跳过 / 0 断言失败。
