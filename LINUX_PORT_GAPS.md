# Linux 移植遗留差距清单

- **基线**：`linux-migration` 分支，tag `v0.8-linux-dv-rpu`（pkgrel 29，2026-09-10）
- **对照**：fork Windows 版（AlkaidLab master `3d76d35d`）的全部增强功能
- **用途**：待办工作清单。每项含现状钩子（代码位置）、技术路径、预估工作量、验收标准
- **约定**：🔶 部分实现（有偏差）｜○ 未实现但可行｜⛔ 本质不可移植

---

## 总览

核心体验链路已全部对齐：虚拟显示器（原生 EDID 后端 + 6 模式 prep + 独占还原）、物理显示器
display_device（kscreen 分辨率/HDR/主屏/拓扑 + 持久化还原）、HDR 三件套（静态透传 / HDR10+
动态 / DV P8.1 RPU）、剪贴板文本双向、远程麦克风、ABR 前台检测、托盘/向导/打包。

剩余：**8 项部分实现、10 项可行未做、9 项本质不可移植**。

---

## 一、🔶 部分实现（有已知偏差）

### 1.1 虚拟显示器 per-client 隔离

- **现状**：客户端切换时**销毁重建** VDD（`session.cpp` 的 `rebuild_old_vdd_id` 链路），
  单虚拟屏共享；Windows 是多实例并存，每客户端独立 GUID（`vdd_utils::generate_client_guid`）。
- **差距**：两个客户端无法同时各用各的虚拟屏。
- **若要做**：Linux 后端需要支持多个 EDID override 连接器并存（每个客户端占一个空闲物理
  连接器），`create_vdd_monitor` 带 GUID 参数化显示名。受限于机器上的空闲连接器数量。
- **工作量**：高。**验收**：两客户端同时各串各的虚拟屏互不干扰。

### 1.2 VDD EDID 模式表 —— ✅ 已完全解决（2026-09-11，`4ad74c90`，tag `v0.9.0-linux-full-mode-table`，pkgrel 37）

**链式 CTA 扩展块**突破单扩展块 6 模式上限：块 1 承载数据块 + 首选时序副本 + 4 个附加模式，
后续 DTD-only 块各承载 6 个——配置分辨率 × 刷新率的全部可行组合（像素时钟可行性过滤）都能
进 EDID。用用户真实配置验证：384 字节 / 2 扩展块 / 10 模式 / edid-decode 零告警，1080p 的
60/90/120/144 全档可见。Range Limits 描述符同步覆盖全部通告刷新率（此前写死 preferred±20
导致 40-80Hz 与 144Hz 模式矛盾）。**全组合达成（`fbb1b911`，tag `v0.9.1`，pkgrel 38）**：非首选分辨率同样携带全部配置
  刷新率（先前每分辨率仅最高可行档）。用户配置验证：640 字节 / 4 扩展块 / 24 DTD /
  edid-decode 零告警——4K@60 首选（144/120/90 超像素时钟上限被可行性过滤）+ 3440x1440@90/60
  + 其余分辨率 @144/120/90/60 全档。
**剩余小项**：生成器空白模型（8%/2.5%）比 CVT-R 保守，
个别真实模式（如 3440x1440@120）被排除——校准后可再放宽。

- **已完成**：手动创建参数化——首选模式从 WebUI 可编辑的 `resolutions`/`fps` 列表推导
  （最高分辨率×最高刷新率），EDID 阶梯携带全部配置档位供合成器免重写热切；客户端会话
  模式仍优先（`cached_from_session`）。无新增配置键。
- **剩余小项**：阶梯上限 6 个模式（EDID 空间限制，Windows SETMODES 无此限；策略 = 首选
  分辨率保留全部配置刷新率，其余分辨率各取最高可行档，`49800f6c`，tag `v0.8.7`，pkgrel 36）；
  会话外 kscreen-doctor 热切已可用但未在 UI 暴露入口；生成器的空白模型比 CVT-RB 保守
  （如 3440x1440@120 被可行过滤器排除，实际该模式合法）——校准空白模型可再放宽。

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

### 2.1 HDR Vivid T.35 序列化器（中）⭐ 推荐下一个

- **现状**：avcodec 路径的 Vivid side data 已预挂并被逐帧更新（`video.cpp:3073` 起），
  但 FFmpeg 没有 CUVA 序列化器（`dynamic_hdr_vivid.c` 只有解析），元数据到不了码流。
  Windows 端由 `nvenc_base.cpp` 手写 T.35 载荷（仅 NVENC 直连路径可用）。
- **路径**：照 `nvenc_base.cpp` 的 T.35 手写逻辑 + fork 自研比特流层
  （`src/cbs.cpp` / `video_hdr_bitstream.cpp`，DV RPU 写入器同模式），写一个
  `av_dynamic_hdr_vivid → SEI(NAL type 39/40 prefix)` 序列化器，在 encode_avcodec 的
  输出包上拼接（同 DV RPU 注入的管线位置）。
- **验收**：Vivid 客户端（支持 GB/T 46269 的播放器/电视盒子）能解出逐帧 Vivid 元数据；
  主机日志无 Vivid 相关警告。

### 2.2 剪贴板图片 + 大文件（中）⭐ 与 2.1 二选一起步

- 见 1.4 的路径。拆两步：先 wlr-data-control 图片双向（PNG），再 blob store 的 KIND_REF。
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

### 2.6 非 KDE 前台检测（低）

- X11：XFixes + `_NET_ACTIVE_WINDOW`/`_NET_WM_PID`（X11 库已链接）；
  wlroots 系：wlr-foreign-toplevel-management。接入 `foreground_app.cpp` 的缓存即可，
  KWin 脚本路径保持优先。
- **验收**：Hyprland/X11 会话下 ABR 日志出现前台切换。

### 2.7 AI Key libsecret 后端（低）

- 见 1.7。

### 2.8 DS5 本地触觉播放 sidecar（高）

- Windows `ds5/ds5_sidecar_client.cpp` + `mic_write` 的触觉 PCM 在主机直连手柄上播放。
- **路径**：`/dev/hidraw` 直写 DualSense 输出报告（udc3 打开 + 触觉报告格式），
  权限走 udev tag。基础 rumble 已由 inputtino 覆盖，这里缺的是 authored IR PCM 高保真回放。
- **验收**：直连 DS5 玩支持触觉的游戏，波形与 Windows 版一致。

### 2.9 NVENC SDK 直连（高，建议重估后可能降级/放弃）

- HDR10+/DV 完成后，avcodec 路径功能面已齐。直连的剩余增益：fork 的细粒度码控、
  lookahed、多硬件实例。若 2.1 Vivid 也完成，T.35 手写序列化器反而是直连的*负资产*
  （avcodec 不需要它）。
- **建议**：搁置，等实际画质/延迟瓶颈出现再评估。

### 2.10 Tauri 控制面板选择性移植（高，收益低）

- 面板约 20 个系统模块仅 Windows 实现（vdd/vigem/vmouse/rtss/hwinfo(WMI)/elevation/
  注册表自启/dualsense elevated/shell 右键菜单等）。WebUI 已覆盖大部分。
- **若有需求**，只值得挑：QR 配对、实时监控（hwinfo/RTSS → Linux 对应物是
  `nvidia-smi`/`amdgpu_top`/`_PIPEWIRE` 统计）、文件右键共享（KDE ServiceMenu 更自然）。
- **建议**：搁置；文件右键共享若想要，走 KDE ServiceMenu（低工作量）单独立项。

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
