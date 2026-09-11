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

剩余：**7 项部分实现（1.2 已解决，不再计入）、10 项可行未做、9 项本质不可移植**。

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
可行性过滤器排除——校准空白模型可再放宽；会话外 kscreen-doctor 热切已可用但未在 UI 暴露入口；
`VddEdid.MatchesReference1080p60Hdr` 的字节参考向量仍是旧的 40–80 Hz Range Limits，需按新语义
重生成（差异仅 2 字节 `0x28,0x50`→`0x37,0x41`，其余 6 个 EDID 用例通过）。

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

- **现状**：avcodec 路径的 Vivid side data 已预挂并被逐帧更新（`video.cpp:3104-3105` 起），
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
