# Foundation Sunshine Linux（Arch Linux）迁移报告

- **基线**：`linux-migration` 分支（pkgrel 66，2026-10-22；已并入上游 `origin/master` `7a0995e2`，
  上一个同步点 `ed415026`，tag `v0.9.3-linux-upstream-sync` 指向 `0e2a6a11`）
- **对象仓库**：`foundation-sunshine`（LizardByte/Sunshine 的 AlkaidLab 增强分支）
- **目标环境**：Arch Linux / CachyOS（x86_64_v3，KDE Plasma + Wayland 与 niri 双目标，PipeWire，
  NVIDIA RTX 3050 Mobile / Ampere sm_86）
- **用途**：本文档记录**当前状态**——Linux 侧已具备什么、证据在哪、边界在哪。
  待办与未完成项见 `LINUX_PORT_GAPS.md`；构建/测试/约定的操作说明见 `AGENTS.md`。

---

## 一、总体结论

**迁移已完成，功能面与 Windows 增强分支基本对齐。** 核心串流能力（KMS/Wayland/X11 捕获、
NVENC/VAAPI/Vulkan/软件编码、evdev 输入注入、WebUI）是上游成熟代码；fork 的 Windows 增强
（虚拟显示器、物理显示器 display_device、HDR 动态元数据三件套、剪贴板主机同步、远程麦克风、
ABR 前台检测、托盘）已逐项移植到 Linux 并补齐了 Windows 对齐审计。

已移植项的清单与证据见 §六；剩余缺口（含 6 项部分实现、9 项可行未做、本质不可移植项）
在 `LINUX_PORT_GAPS.md` 逐条编号维护。

**上游同步状态**：2026-10-22 已并入 `origin/master` `7a0995e2`（33 个提交，403 文件）。
其中两大跨平台重构（Simple-Web-Server → 自带 Beast HTTP 栈、confighttp 拆成 24 个模块）在 Linux
上零改动直接生效。本次同步引入的唯一功能缺口——管线帧率的"源更新"段没有 Linux 后端
（`LINUX_PORT_GAPS.md` §7.1）——也已补上 KMS 一侧，其余为上游测试的环境依赖。

**已知总边界**：CI 目前只有 Windows 构建 job（`.github/workflows/main.yml` 的
`setup_release` / `vdd_smoke` / `build_win`），Linux 编译与测试依赖本机手工验证。

---

## 二、平台支持现状速览

| 层面 | 现状 |
|---|---|
| Linux 平台代码 | `src/platform/linux/` 文件齐全（kmsgrab / wlgrab / x11grab / vaapi / cuda / wayland / graphics / audio / input），为上游成熟代码，无致命 stub |
| FFmpeg 依赖 | 用子模块 `third-party/build-deps/dist/Linux-x86_64` 的静态库（avcodec/avutil/swscale + x264/x265/SVT-AV1/hdr10plus/cbs） |
| Boost | 要求**精确 1.92.0**；Arch 当前即 1.92.0。版本不匹配时 CMake 自动 FetchContent 联网下载（`cmake/dependencies/Boost_Sunshine.cmake`）；系统包路径下 `Boost::format` 可能未定义，已补指向 `Boost::headers` 的 INTERFACE 目标 |
| WebUI | Vue/Vite，`npm run build` 交叉生成静态资产，跨平台 |
| 托盘图标 | 需要 `libayatana-appindicator` + `libnotify`；`SUNSHINE_REQUIRE_TRAY` **默认 ON**，缺失时直接 FATAL |
| CUDA | **默认 ON**（`cmake/prep/options.cmake:60`）；缺失工具链只是跳过 CUDA 源。CUDA 13.4 / sm_86 实测编译+链接通过 |
| CI | 仅 Windows（`setup_release` / `vdd_smoke` / `build_win`），无 Linux job |
| 打包 | `packaging/arch-local/`（本机调试包，装已构建树到 `/usr`）+ `packaging/aur/sunshine-foundation-git/`（AUR 从源码构建）。上游旧布局的 `packaging/linux/` 模板（desktop/appdata/service）已恢复 |

---

## 三、构建与打包

### 构建

```bash
sccache --start-server 2>/dev/null || true
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/sunshine -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d .)" \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CUDA_COMPILER_LAUNCHER=sccache -DBUILD_TESTS=ON
ninja -C build sunshine
cd build && ctest
```

- `SUNSHINE_ENABLE_CUDA` 在 Linux 默认 ON；缺失 CUDA 只是跳过 CUDA 源。
- 显式指定 `CMAKE_CUDA_ARCHITECTURES` 只编本机架构；未指定时走兼容阶梯，阶梯会自动剔除
  工具链已删除的架构（CUDA 13 不接受 Turing 以下）。
- 链接期 `libcuda.so.1`（NVIDIA 驱动）是 `dlopen()`，cudart 静态链接——**运行时不需要 CUDA 工具包**。

### 打包

`packaging/arch-local/`（本地调试包，不在 makepkg 内编译）流程：bump `pkgrel` →
`cmake -B build ...`（刷新二进制版本戳）→ `ninja -C build sunshine` → `makepkg -f`。
`SUNSHINE_BUILD_DIR=<dir> makepkg -f` 可打包别的构建树。**不要**在 ninja 仍在链接时跑 makepkg；
**不要**提交 `packaging/arch-local/{pkg,src,*.pkg.tar.zst}`。

`packaging/aur/sunshine-foundation-git/` 从源码构建，每个构建需要的子模块都是固定提交的
`source` 条目（makepkg 不检出 gitlink 且 `build()` 无网络），`prepare()` 把源码树还原到 gitlink
路径，并对 `third-party/AMF`（Windows-only，仅保留头）与 `third-party/build-deps`（只留本机
`dist/Linux-<arch>` 切片）做 sparse-checkout。默认 `_use_cuda=false`（`cuda` 是 AUR 包，不进基础依赖）。

---

## 四、Arch Linux 依赖清单

### 构建依赖（pacman 包名）

| 包 | 用途 |
|---|---|
| `base-devel` | 编译工具链 |
| `cmake` (≥3.25) / `ninja` / `git` | 构建系统与子模块 |
| `boost` 1.92.0 | 精确版本要求 |
| `openssl` / `curl` / `miniupnpc` / `opus` | 网络与音频编解码 |
| `libdrm` + `libcap` | KMS 捕获与文件能力 |
| `libva` | VAAPI 编码 |
| `wayland` + `wayland-protocols` | Wayland 捕获（protocols 默认用子模块） |
| `libx11` + xcb/xrandr/xtst/xfixes/xi/xinerama | X11 捕获 |
| `libpulse` | 音频捕获/播放（PipeWire 的 pulse 兼容层） |
| `libsecret`（可选） | AI API Key 存 keyring（gnome-keyring / KWallet 的 secret-service 兼容层）；缺省退回 `SUNSHINE_LLM_API_KEY` |
| `libevdev` | 输入注入（inputtino） |
| `numactl` | 链接静态 FFmpeg 需要 libnuma |
| `libayatana-appindicator` + `libnotify` | 托盘（`SUNSHINE_REQUIRE_TRAY` 默认 ON） |
| `node` ≥26.7 且 `<27`、`npm` ≥11.19 且 `<12` | WebUI 构建（`package.json` engines 约束） |
| `cuda`（可选） | NvFBC/CUDA 捕获路径（NVENC 不需要它） |
| `qt6-base` + `qt6-svg` | 托盘 Qt6 实现 |
| `sccache`（可选） | 编译缓存，重建提速显著 |

```bash
sudo pacman -S --needed cmake ninja boost numactl \
  libayatana-appindicator libnotify qt6-base qt6-svg \
  libdrm libcap libva wayland libx11 libxcb libxrandr libxtst libxfixes libxi libxinerama \
  libpulse libevdev miniupnpc opus curl openssl libsecret
```

### 运行时依赖

上表库的运行时包 + `nvidia-utils`（NVENC 无需额外 SDK）+ `pipewire`（经 pulse API 供音频）。
安装后按 §五做 `setcap`/udev 配置。

---

## 五、运行时配置

1. **KMS 捕获权限**（Wayland 下的主要整屏捕获路径）：
   ```bash
   sudo setcap cap_sys_admin,cap_dac_read_search,cap_dac_override,cap_sys_ptrace+p \
     $(readlink -f $(which sunshine))
   ```
   虚拟显示器后端还需要 `cap_dac_read_search`/`cap_dac_override`（debugfs/sysfs 写入）与
   `cap_sys_ptrace`（向合成器借 DRM master）；进程内用 libcap RAII 把 permitted 提升为 effective。
   包 post-install hook 会自动执行这一步。
2. **输入设备权限**：`60-sunshine.rules` 给 `/dev/uinput`、`/dev/uhid` 打 `uaccess` 标签。
3. **systemd 用户服务**：`systemctl --user enable --now sunshine`。
4. **捕获方式现实（KWin Wayland）**：KWin 不支持 wlr-export-dmabuf/wlr-screencopy，故 `wlgrab` 在
   Plasma Wayland 上不可用；`x11grab` 只能抓 XWayland 窗口；实际可行的是 `kmsgrab`（需第 1 步
   的 setcap）。niri 下 VDD 输出由 DRM 层点亮。
5. **NVIDIA**：确认 `nvidia-drm.modeset=1`。NVENC 由 FFmpeg 运行时加载驱动。
6. **端口**：TCP 47984–47990、48010；UDP 47998–48000。Arch 默认无防火墙。
7. **音频**：PipeWire 经 `pipewire-pulse` 兼容 libpulse。

---

## 六、已移植的 Windows 增强

> 每项给出当前实现位置与关键证据行号。历史演进细节不再保留在本文档。

### 6.1 虚拟显示器（ZakoVDD → 原生 Linux 后端）

**形态**：in-process 原生后端（不使用外部守护进程或 helper 二进制）。个性化 EDID + 连接器状态
强制 + pidfd 借 DRM master 做 CRTC 指派 + 独占模式物理屏还原。

- **EDID 生成**：`src/platform/linux/vdd_edid.{h,cpp}`（340 行）。证据：
  `vdd_edid.h:24` 像素时钟上限 `kMaxDtdPixelClockHz = 655350000.0`（DTD 16 位 ×10 kHz）；
  `:49` `edid_options.extra_modes`；`:66` `mode_fits_pixel_clock_limit()`；
  `vdd_edid.cpp:55` `dtd_pixel_clock_hz()` 单一来源、`:103-104` 可行性过滤、`:178` 饱和保护、
  `:221` 附加模式布局。显示名为共享常量 `ZAKO_NAME`（"Zako HDR"），KDE 前缀的 "UQD" 是 EDID
  厂商 ID 字节的固有解码，非命名分歧。
- **完整模式表（链式 CTA 块）**：块 1 = 数据块 + 首选时序副本 + 4 个附加模式，后续 DTD-only 块各 6 个
  （逐块校验和与 DTD 起始指针），通告**配置分辨率 × 刷新率的全部可行组合**，与 Windows SETMODES
  的"全组合"语义等价。阶梯仅保留防病态配置的 40 项上限（`src/display_device/vdd_utils.cpp:1990-1991`）。
  Range Limits 描述符覆盖全部通告刷新率。
- **连接器与 CRTC**：`src/display_device/vdd_utils.cpp` Linux 段——
  `:2315` `set_vdd_session_mode()`、`:1501` `force_crtc_assignment()`（pidfd 借 master + dumb buffer +
  `drmModeSetCrtc`）、`:1424` `wait_for_compositor_output()`、`:2070` `run_logged()`（有界 fork/exec，
  关闭继承 fd，10 s 上限）、`:1196` `restore_offlined_physicals()`、`:2064` `offlined_physical_connectors()`。
- **组合器先行、抢 master 兜底**：status 强制上电触发的 uevent 会让 KWin 自己探测并点亮连接器，但该
  激活是一次性的——抢 master 若与之相撞，KWin 的 atomic commit 失败后丢弃该输出且永不重试。因此
  创建与模式切换路径先轮询 kscreen 等组合器点亮（`:2235`），只有组合器忽略 status-forced 连接器时才
  走 `:2237` 的兜底，并在兜底后对可观察组合器做一次 status off→on 循环给组合器新的激活机会。
- **残留 adopt**：EDID override 与强制连接器状态是 DRM 持久的（进程内簿记随进程消失）。
  `adopt_orphan_vdd_locked()`（`:1871`）扫描带 VHD 签名 EDID 的连接器并恢复跟踪，
  `live_virtual_display_connector()`（`:2044`）是其入口——**不要**重建它能 adopt 的虚拟屏。
- **模式发布校验**：`is_mode_advertised()`（`:2161`）查询合成器真实模式表而非"VDD 活着即通过"；
  `wait_for_mode_publication()`（`:2185`）按 `kModePublicationTimeout` 轮询，失败时打印实际发布的
  模式列表并返回 `modes_fail`；查询不可用（niri/wlroots/X11）视为"未知"。
- **prep 模式**：`device_prep_e` 的 5 个取值（`src/display_device/parsed_config.h:27-33`）映射到
  `vdd_prep_e`（`:126-131`）。`23172`/`ZAKO_NAME` 设备 id 特判 `explicit_vdd`。

### 6.2 物理显示器 display_device（kscreen + niri 后端）

`src/platform/linux/display_device.cpp` 以合成器为中介读写模式/HDR/主屏/拓扑，JSON 持久化 +
apply/revert 编排对齐 Windows `settings.cpp`。合成器有两个生产者：kscreen-doctor（Plasma）与
niri IPC（见 §6.16），由 `query_outputs()` 按会话分派。

- `:143` `query_outputs()`、`:706` `set_display_modes()`、`:913` `set_hdr_states()`、
  `:1035` `set_topology()`、`:360` `wait_for_topology()`、`:383` `wait_for_display_stability()`。
- 模式匹配用 Windows 的 1 Hz 模糊比较 + 取最近候选；失败时按三段式回滚而非留下半套模式。
- 空 device_id（Display: Auto）解析为主屏（kscreen priority 1），模式/HDR 不再作用于所有输出。
- 拓扑校验与 setter 一致地拒绝多设备组（复制组当前不可表达，见 `LINUX_PORT_GAPS.md`）。
- kscreen 不可用的环境退化为既有的 no-op 行为并打 **warning**（明确"显示设置未被应用"），
  但保留 success 结果——这是 Linux 上会话的持久属性，返回失败会让所有非 KDE 会话进入无法修复的
  deferred-retry。niri 会话不进入这条降级：模式与拓扑开关由 §6.16 的 IPC 后端接管，只有 HDR 与
  主屏两项因 niri 没有对应接口而跳过（记录 warning，同样不返回失败）。

### 6.3 HDR 动态元数据三件套（avcodec 路径）

静态透传本就跨平台：`src/platform/linux/kmsgrab.cpp:864` 读 DRM `HDR_OUTPUT_METADATA` blob、
`:930` 解析，`src/video.cpp:3443` 挂 mastering display side data、`:3462` 挂 content light。

动态部分由 CPU 亮度分析器供给统计：

- **分析器**：`src/video_hdr_analyzer.h` 的 `video::hdr_analyzer::analyze()`——**逐像素
  max(R,G,B)** 的 PQ 直方图（与 Windows 计算着色器同一个量，见 §6.20）；`src/video.cpp` 只把
  `AVFrame` 填成 `frame_t` 再调用（`analyze_pq_frame()`），软件设备路径与硬件设备采样下载路径各
  一处。平面 10-bit 格式按格式掩码解包（P010/YUV444P16 为 MSB，YUV420P10LE/YUV444P10LE 为 LSB）。
- **节奏与上限**：共享常量 `hdr_analysis_interval = 4`（`src/video_hdr_metadata.h:330`，对齐 Windows
  的 1/4 帧采样）、`hdr_analysis_max_width/height = 1920/1080`（`:337-338`）、
  `st2084_peak_nits = 10000.0f`（`:344`）。
- **HDR10+**：`src/video.cpp:2581` `update_hdr_dynamic_metadata()`；side data 懒挂载——第一帧有效
  统计到来才创建，消除占位 SEI。
- **DV P8.1 RPU**：`src/video.cpp:2808` `dolby_vision_.stage()`（avcodec 路径），NVENC/AMF 直连路径
  为 `:1257`/`:1386`；按提交帧序 stage L1、输出包按 pts 回程 splice RPU NAL。
- **HDR Vivid**：FFmpeg 只有 CUVA 解析器、无序列化器，故在 `encode_avcodec()`（`:2724`）内用共享
  `serialize_vivid_t35()`（`:2784`）构建 T.35，经 `hdr_bitstream::append_t35_unit()`（`:2785`）按 pts
  拼到首个 VCL NAL 之前。**仅 Linux 编译**——Windows 的 avcodec 家族显式标记不能承载 Vivid。
- **ST2084 常量唯一来源**：`src/video_hdr_metadata.h:26-30`（`st2084_m1/m2/c1/c2/c3`）；
  `:366` `pq_to_nits()`。**不要重新推导 PQ 数学**。

### 6.4 剪贴板主机同步（klipper D-Bus，KDE 上的文本通道）

`src/clipboard_host.cpp`（457 行）经 sd-bus 对话 KDE klipper
（`org.kde.klipper` set/getClipboardContents），与客户端双向同步，复用 GUI agent 的线协议。

- **线协议**：`src/clipboard_wire.h`（185 行）编解码、`src/clipboard_echo.h`（123 行）16 项
  `(kind, payload-hash)` + TTL 的回声环形。常量收敛在 `src/clipboard_bridge.h`
  （`:81` `inbound_sink_fn`、`:94` `set_inbound_sink`、`:98` `add_inbound_listener`）。
- **AT_SECURE 下的用户总线**：Sunshine 带文件能力运行 ⇒ `secure_getenv` 隐藏 `XDG_RUNTIME_DIR`，
  `sd_bus_open_user()` 失败。用 `platf::sdbus::open_user_bus()`
  （`src/platform/linux/sdbus_session.h:18`）——它必须调 `sd_bus_set_bus_client(bus, 1)`（`:35`），
  否则 socket 连上但从不发 `Hello()`，dbus-broker 对第一条方法调用直接断开。
- **不阻塞控制线程**：客户端→主机的写入是有界队列（8 条，溢出丢最旧），由 provider poll 线程执行；
  等待用条件变量。klipper 变更信号 `clipboardHistoryUpdated` 事件驱动 + 1 s 兜底读。
- **边界对齐 agent**：出站过滤空读取；入站丢弃空串与含 NUL 文本。klipper 只有字符串，图片由
  §6.14 的 data-control 模块承担，两者共用同一套帧；大文本与大图都走 blob store 的 `kKindRef`
  描述符帧（`kInlineThresholdBytes = 60'000`）。`clipboard_image` 能力按 §6.14 的实际可用性通告。
- **只在有 klipper 时才走 klipper**：启动时 `klipper_present()`（一次 `klipper_get` 探针）判定；
  桌面没有 klipper（niri / sway / Hyprland / plain weston）且 data-control 可用时，`ext_text` 置位，
  **文本改由 §6.14 的合成器通道承载**（`poll_loop` 此时只负责刷 `notify_gui_alive()` 存活戳）。
  在此之前，非 KDE 桌面上的文本剪贴板双向都不工作——klipper 是当时的唯一文本源。

### 6.5 远程麦克风写主机

`src/platform/linux/audio.cpp`：命名 null-sink `sink-sunshine-virtual-mic`（`:216`），monitor 源即
虚拟麦克风；`:647` `write_mic_pcm()`、`:676` `init_mic_redirect_device()`。

- **背压契约**：`:227-237` 有界队列 + writer 线程（`src/platform/linux/mic_queue.h`，116 行，
  容量 5 帧 = 100 ms）。`write_mic_pcm()` 只入队、永不阻塞共享混音线程；队列满返回 0（约定的背压丢帧）；
  阻塞写与错误码映射（-2 设备丢失 / -1 一般错误）在 writer 线程完成并在下一次调用上报。
- **默认录音设备切换**：`:536` `set_default_source_name()`——读当前默认源 → 指向 null-sink monitor →
  释放时还原；对 PulseAudio/PipeWire 通用。

### 6.6 ABR 前台应用检测

`src/platform/linux/foreground_app.cpp`（463 行）。

- **KDE**：`:134` `install_kwin_script()` 经 D-Bus 加载 KWin 脚本（静默自动重装），活动窗口变化推送
  到进程内 `org.sunshine.Abr` 服务；`detect()` 返回缓存。
- **niri**：轮询 `niri msg --json focused-window`（2 s，走有界执行），JSON 逐字段类型检查地映射到
  `info_t`；`null`（无焦点窗口）不更新缓存。
- **后端选择**：启动时先探测 `org.kde.KWin` 是否在会话总线上；KDE 保持 KWin 路径，无 KWin 且 niri
  查询可用才启用 niri；两者都无则保持空结果降级。
- **缓存失效**：`:86` `clear_cache()`——生产者已死（脚本重载失败 / niri 查询失败）时清空，ABR 退回
  基于启动器的分类。

### 6.7 合成器输出启用（桌面分发）

`src/platform/linux/compositor_output.{h,cpp}`：`:28` `enable_command()` 是**纯函数**（桌面串 →
命令字符串），`:12` `niri_session()`、`:18` `tool_available()` 提供探针。KDE → kscreen-doctor、
niri → `niri msg output <c> on`、其他 Wayland → `wlr-randr`（wlr-output-management）、X11 → `xrandr`。

> **该设计是事故驱动的**：早期版本把 KDE 分支误写成调用自己，导致 Plasma 下创建/切换 VDD 必崩
> （自递归栈溢出）。现在映射是纯函数并被单元测试逐分支锁死命令字符串。**不要**把 dispatcher 的分支
> 改回可递归的调用。

### 6.8 非 Plasma 会话不再被 kscreen-doctor 阻塞

`src/platform/linux/kscreen_backend.{h,cpp}` + `kscreen_modes.h`。

- **会话判定**：`session_supports_kscreen()` 先看 `NIRI_SOCKET`（niri 无 KScreen），否则要求 desktop
  串含 Plasma/KDE；**每次重新判定不缓存**，所以先于桌面启动的服务在用户登录后仍能接上。不支持则
  **根本不 spawn**。
- **短探测 + 冷却**：查询用 1.5 s 超时，失败/超时武装 20 s 冷却，成功即清除；变更命令仍 10 s 且只在
  探测成功后发出。
- 实测（`advertised_modes("eDP-1")`）：KDE ~699 ms 不变 / niri 0 ms（不再 spawn）/ 无桌面 0 ms /
  声称 KDE 但合成器不应答首次 807 ms 后续 0 ms。

### 6.9 托盘

`src/tray/system_tray.cpp`：VDD 子菜单（显示名 "Zako HDR"）、每 2 s 轮询 `is_vdd_active()` 并变化即
刷新、首次查询时 adopt 残留虚拟屏、危险操作走 Qt `QMessageBox` 确认（Windows 为 `MessageBoxW`，
整体在 `#ifndef _WIN32` 内）、Advanced Settings 子菜单两平台布局一致（导入/导出/重置/清理缓存/
重置显示器），退出文案为 Linux 专用键。Qt 托盘需 `checkbox=1` 才渲染勾选态（Windows 仅凭 `checked`
画 `MFS_CHECKED`，该字段在 Windows 被忽略）。

### 6.10 NVENC YUV 4:4:4

`src/video.cpp`：`:713` `YUV444_SUPPORT` 旗标、`:1520` nvenc 条目 flags、`:1536` 444 格式填
`AV_PIX_FMT_YUV444P`（8-bit）+ `AV_PIX_FMT_YUV444P16`（10-bit 16-bit 移位容器）、
`:1615` h264 sdr444 补 `AV_PROFILE_H264_HIGH_444_PREDICTIVE`。支持与否由真实试编码探测决定。
CUDA=ON 的 kms-egl 路径为三平面渲染（`egl::create_target()`/`sws_t` 的 `is_planar_yuv444()` 判定）。
**边界**：CUDA=ON 的 ram 回退路径（wlgrab/nvfbc，`cuda::sws_t` 的 `RGBA_to_NV12` CUDA 核）仍
NV12-only；VAAPI 444 未开（见 `LINUX_PORT_GAPS.md` §2.11）。

### 6.11 AI API Key 存进 keyring

`src/ai/credential_store.cpp`：`SUNSHINE_BUILD_LIBSECRET`（CMake 可选探测 `libsecret-1`）下用 Secret
Service 替代 Windows 的 DPAPI blob。schema `org.lizardbyte.sunshine.LlmApiKey`，属性
`application=sunshine` / `purpose=llm-api-key`，每用户一条，写入默认 collection；label
`Sunshine LLM API key`。读取优先 keyring、`SUNSHINE_LLM_API_KEY` 仅作引导兜底（与 Windows 一致：
持久化值优先）。清空密钥走同一入口传空串；条目不存在时视作成功（幂等）——libsecret 对"无匹配"返回
FALSE 且不带 `GError`，这点是实测出来的，别按返回值判错。若 env 里仍留着 `SUNSHINE_LLM_API_KEY`，
擦除会明确报错提示先清理 service 环境，否则 key 仍在生效。没装 `libsecret` 时行为与之前完全一致。
验收用例：`tests/unit/test_credential_store.cpp` 的 `RoundTripsThroughTheKeyring`（无 Secret
Service 或本用户已有 key 时 `GTEST_SKIP`，绝不覆盖真实凭据）。

### 6.13 非 KDE 合成器的前台窗口检测

ABR 的前台应用分类原先只覆盖 KDE（KWin 脚本 + 私有 D-Bus 服务）与 niri（`niri msg --json
focused-window`）。`src/platform/linux/foreground_app.cpp` 现在按能力探测四级 producer，任一级缺席
就落到下一级：

1. KWin（唯一带 pid 的 Wayland 源，`org.kde.KWin` 占着总线名即用）；
2. `wlr-foreign-toplevel-management-unstable-v1`——sway / river / wayfire 等 wlroots 系合成器，
   `state` 事件里的 `ACTIVATED` 直接给出前台窗口，每次 `done` 扫描活动项。协议不带 pid，
   靠 exe 名比对识别切换。协议 XML 复用仓库里已有的 `third-party/wlr-protocols/unstable/`
   （`wlgrab` 用的 export-dmabuf 就在同目录），只加了一行 `GEN_WAYLAND`，没有新依赖；
3. niri 的 IPC（原有路径，探针即那条查询本身）；
4. X11 的 `_NET_ACTIVE_WINDOW`（`PropertyNotify` + 2s 定时重读），字段取 `_NET_WM_PID`、
   `WM_CLASS` 第二段（对齐 KWin 的 resourceClass）、`_NET_WM_NAME`（缺失退 `WM_NAME`）。
   整块在 `SUNSHINE_BUILD_X11` 内，因此不改变未启用 X11 的构建。

没有采用 wayland-protocols 的 `ext-foreign-toplevel-list`：它枚举 toplevel 却不带激活状态，回答不了
“谁在前台”。X11 读取逻辑用自建窗口做过端到端验证（pid / class / 含中文标题三项均匹配，测后还原了
根窗口属性）；wlroots 分支在本机无法运行时验证（只有 Plasma 与 niri，niri 不支持该协议），
只验证到符号级。Hyprland 走自己的 hyprctl，仍需单独路径（见 `LINUX_PORT_GAPS.md` §2.5）。

### 6.12 管线帧率的源更新段（KMS）

`src/platform/linux/kmsgrab.cpp` 的 `display_t` override 了
`frame_rate_source()`（`src/platform/common.h:685`），生产
`perf::capture_source_kind_e::drm_fb_flips`：`refresh()` 每次读到 `plane->fb_id` 后调用
`observe_scanout(fb_id)`，只有**扫描出的帧缓冲变了**才记一帧，语义对齐 Windows 的 DXGI present /
WGC arrival（静态桌面记 0）。这填补了上游戏后"源更新帧率"恒为 `unavailable` 的空白——kmsgrab
的捕获循环按串流帧率定速，屏幕冻住时捕获段照样报 60 fps，源段才能暴露"上游没出新帧"。

驱动的 vblank 计数器在本机（NVIDIA）上不可用：`drmCrtcGetSequence` 与 `drmWaitVBlank` 都返回
`EOPNOTSUPP`（实测），所以没有把绝对刷新率当源速率。`wlgrab`（`wlr-export-dmabuf` 一帧一请求，
源速率与捕获速率恒等）与 `x11grab`（定时轮询、未接 XDamage）没有独立信号源，保持
`unavailable`——理由写在 `LINUX_PORT_GAPS.md` §7.1。

### 6.14 剪贴板 data-control 通道（图片 + 非 KDE 文本）

`src/platform/linux/clipboard_data.{h,cpp}`（`SUNSHINE_BUILD_WAYLAND` 下编译）。klipper 只承载
字符串，图片只能走合成器自己的剪贴板，因此这一半用标准化的 `ext_data_control_v1`（Plasma 6 实现；
wlroots 仍只导出 wlr 拼写，那里会保持 text-only）在**独立 Wayland 连接 + 独立线程**上双向同步 PNG，
并同时承载**文本**（§6.4 的 `ext_text` 模式，供没有 klipper 的桌面使用）。

- **双向语义**：`selection` 事件给出带 `image/png` 的 offer 时 `receive()` 到管道读回字节 →
  `clipboard_host` 的回声环/会话检查后发布；反向由客户端帧驱动，建 data source 宣告 `image/png` 并
  `set_selection()`，粘贴时合成器通过 `send()` 取字节，直到别的客户端接走选择（`cancelled`）。
- **回调里绝不阻塞**：`send()` 不直接写管道（大图不装进管道缓冲会死锁事件循环，而事件循环正是
  必须把水排掉的那一方），改为登记 `out_transfers`；读取侧同样以 `recv_fd` + 非阻塞 fd 挂进
  `poll()`，统一带 10 s 截止时间。首版就是踩了这个坑：自发布回读时阻塞读与 `send()` 互锁，
  表现为 `transfer timed out after 0 bytes`、外部 `wl-paste` 也拿不到数据。
- **实测**（Plasma 6，`wl-copy`/`wl-paste` 对照）：594 B、45 KB、3.4 MB 三种尺寸，`us → desktop`
  由 `wl-paste` 取回、`desktop → us` 由 harness 收回，md5 全部一致；自发布回读亦正确。
- **文本风味协商**：`src/platform/linux/clipboard_mime.h`（纯 header，无 Wayland 依赖）决定向 offer
  要哪一种表示——`image/png` 优先；文本按 `text/plain;charset=utf-8` → `UTF8_STRING` →
  `text/plain` → 其它 `text/*` 排序；带非 UTF-8 字符集的表示**拒收**（照原样当 UTF-8 会变乱码），
  不可承载的 offer（`application/*`、`image/svg+xml`、X11 的 `STRING`/`COMPOUND_TEXT`）则一个也不
  请求，宁可不动客户端剪贴板也不清空它。发布文本时同时宣告三种 UTF-8 拼写，`send()` 接受其中任意
  一种。策略由 `tests/unit/test_clipboard_mime.cpp` 10 例固定。
- **文本实测**（Plasma 6，与 klipper 并存）：`set_text("sunshine-ext-text-probe-42")` 后
  `wl-paste --no-newline` 逐字取回同一字符串；外部 `wl-copy` 写入后 sink 收到 `from-wl-copy-77`，
  自发布回读同样会进 sink——这正是 `clipboard_host` 用 `last_text` + 回声环双重去重的原因。
- **能力通告**：`src/rtsp.cpp` 的 `clipboard_image` 不再被 `#ifdef _WIN32` 挡死，改问
  `clipboard_host::image_available()`——协议缺失时仍不通告，客户端不会发出无处落地的帧。
- **未做**：file-offer（`kKindFileOffer`）仍只属于 Windows GUI agent；Linux 侧要支持需接
  xdg-desktop-portal 的文件选择器，记为 `LINUX_PORT_GAPS.md` §2.1b。

### 6.15 远程 USB 的 usbip-utils 主机后端

`src/remote_usb/remote_usb_host_controller.cpp` 新增 `usbip_host_backend::usbip_linux` 方言，
`automatic` 在 Linux 解析到它（Windows 仍是 `usbip_win2`）。生命周期、并发上限、取消、stop 时的
补偿 detach 全部沿用原有跨平台实现，只有两处平台差异：

- **argv**：attach 用 `usbip --tcp-port PORT attach -r HOST -b BUSID`（`--tcp-port` 是全局选项，
  必须写在子命令前），detach 用 `usbip detach -p PORT`；win2 的 `--once/--terse` 在 usbip-utils
  上不存在。
- **端口来源**：usbip-utils 成功时不打印任何内容，端口从内核 vhci 的
  `/sys/devices/platform/vhci_hcd.0/status` 反推——attach 前快照、attach 后取差集，50 ms × 20 次
  有界轮询（内核探针滞后于工具返回）；窗口用文件级互斥串行化，避免并发 attach 认领同一端口。拿不到
  新端口即 `attach_failed`，不留无法 detach 的绑定。vhci 端口是 0 基，绑定沿用 win2 的 1 基表示
  （内部 `port + 1`）。

`backend_supported()` 现在要求 `usbip` 可执行文件存在且 vhci 状态文件可读，任一缺失即回到
`unsupported` 并让 reverse tunnel 保持关闭。部署上仍有一道硬门槛：vhci 的 `attach`/`detach` 属性
只有 root 可写，而本仓 service 以用户身份运行，所以端到端启用需要 root 运行或给 `usbip` 配 sudoers
包装；权限被拒时 detail 会显式指出这一点。单测覆盖 argv 约定、差集推导、0 基端口、无新端口失败与
状态缺失降级。

### 6.16 niri 输出后端（IPC）

`src/platform/linux/niri_ipc.{h,cpp}` + `src/platform/linux/display_device.cpp`。niri 不实现
KScreen 也不实现 `wlr-output-management`（实测 registry 无 `zwlr_output_manager_v1`），但自带
`niri msg` CLI + Unix socket（`NIRI_SOCKET`）与 JSON 输出，因此作为第二个 display_device 生产者。

- **会话判定**：`session_supports_niri()`——`NIRI_SOCKET` 存在即真（niri 会导给所有子进程，即使
  `XDG_CURRENT_DESKTOP` 为兼容性写成 KDE）；声明 KDE/Plasma 且无 socket 即假（不扫目录）；否则扫
  `$XDG_RUNTIME_DIR` 与 `/run/user/<uid>`（服务用文件能力运行时 `secure_getenv` 会隐藏前者）下的
  `niri.*.sock`，按 mtime 新→旧最多探 4 个，`niri msg --json version` exit 0 即认定。
- **有界执行**：读 1.5 s、写 10 s（`vdd_utils::run_logged`，fork/exec + 关 fd + 超时 kill）；
  失败后 20 s 冷却，避免每次查询都拉一个进程。
- **读**：`niri msg --json outputs` → `platf::niri::parse_outputs()`（纯函数，`refresh_rate` 的
  milliHz 转 Hz、`current_mode` 是模式下标、`null` 表示输出关闭）→ 映射成 `compositor_output_t`。
  niri 没有 mode id，`id` 用下标字符串代替。
- **写**：`niri msg output '<name>' mode WxH@R` / `on` / `off`。**`niri msg` 无论请求是否被接受都
  exit 0**（未连接/非法模式只打一行 stdout），所以 `niri_change()` 与 kscreen 一样回读状态判定，
  最多 3 次、间隔 500 ms。
- **HDR / 主屏**：niri 的 IPC 两者都没有，`hdr_state_str` 恒空、`set_as_primary_device()` 返回
  false 并打一次性 warning；`apply_config` 对这两项显式容忍（不把会话判失败）。
- **测试**：`tests/unit/test_niri_ipc.cpp` 11 例覆盖 milliHz 换算、`current_mode` 下标、输出关闭、
  越界下标、缺字段默认值、非对象/坏 JSON 拒绝。

### 6.17 DSU 运动数据服务器（cemuhook）

`src/dsu_server.{h,cpp}` 从 `src/platform/windows/` 挪到顶层共享源（`cmake/sources/host.cmake`），
平台差异只剩一处：`WSAIoctl`/`SIO_UDP_CONNRESET` 的初始化块包在 `#if defined(_WIN32)` 内。协议
实现本身（16 字节 header、CRC32 自校验、INFO/DATA 两种消息、100 字节数据包、客户端超时清理）
一行未改，Windows 预处理结果与合并前逐字相同。

- **数据源**：Moonlight 客户端经 `gamepad_motion_t` 发来的 `LI_MOTION_TYPE_ACCEL` /
  `LI_MOTION_TYPE_GYRO`。挂载点在 `src/platform/linux/input/inputtino_gamepad.cpp` 的 `motion()`，
  与 Windows 的 `dsu_send_motion` 同源。**不是**宿主 IMU —— 早先 gap 文档判断错了。
- **惰性启动**：首个 motion 事件才 `init_dsu_server()`，受 `config::input.enable_dsu_server`
  （默认 false）门控；`start()` 失败即销毁并只告警一次，与 Windows 的生命周期逻辑一致。
- **单位与轴向**：转发客户端原始值（m/s²、deg/s），未到达的那一半传 0，服务保留上一次的值；
  服务端按 cemuhook 约定重排轴向（X=−x、Y=z、Z=−y、Yaw=−y）。
- **配置**：`enable_dsu_server` / `dsu_server_port`（默认 26760）是共享配置键，WebUI 的开关与端口
  在 Linux 上直接生效，无需单独接线。
- **既有怪癖（未改）**：服务端从请求偏移 8（header 的 crc 字段）取"客户端 ID"并回显，而非
  cemuhook 文档的偏移 12。真实客户端一律填 0 所以从未暴露；测试按实际行为断言，避免为"修 bug"
  改动 Windows 也依赖的字节布局。
- **测试**：`tests/unit/test_dsu_server.cpp` 9 例，真实回环 UDP，覆盖 INFO 响应字段、CRC32、
  未订阅不发数据、订阅后收包、加速度/角速度轴向、分次到达的保留语义、未知消息类型、重启。

### 6.18 GNOME Mutter 显示后端

`src/platform/linux/mutter_display.{h,cpp}` 直连 `org.gnome.Mutter.DisplayConfig`（session bus），
`display_device.cpp` 的分发顺序变为 `niri → mutter → kscreen`，三者都不在时仍退化为 no-op 加一条
warning。Mutter 的模型与另外两个后端差别很大，因此接入点全部按它的语义重写：

- **整份替换、一次原子写入**：`GetCurrentState` 拿 serial + 全量配置，改完 `ApplyMonitorsConfig`
  （method 2 = persistent）。没有 per-output 动词，所以 `mutter_change()` 把
  "读 → 改 → 写 → 回读" 串成一个模板函数，3 次重试 / 500 ms，`mutate` 返回 false 表示这条改动
  根本无法表达（如未知 connector）→ 立即停止重试。
- **mode id 是字符串**：Mutter 只接受它自己发出去的 id（`2560x1440@144.001` 这种），`compositor_mode_t::id`
  因此存原串而不是像 niri 那样用下标；改模式 = 把选中的候选标成 `current` 再整份写回。
- **禁用 = 从 logical monitor 里摘连接器**，摘空的 logical monitor 一并删除；启用 = 追加一个
  logical monitor，位置排在现有桌面右边缘（`mutter_right_edge()` 按各 logical monitor 的实际宽度
  换算逻辑坐标）。没有任何 logical monitor 时补一个 primary。
- **主屏是 logical monitor 的 flag**，所以 `set_as_primary_device` 在 Mutter 上能表达与 Windows
  相同的语义（恰好一个 primary），不像 niri 只能退化成"第一个输出"。
- **HDR = color-mode**：1 = `BT.2100`（HDR），0 = default（SDR）；`supported-color-modes` 不含
  BT.2100 时明确报错而不硬写。`hdr_state_str` 只在显示器声明支持 HDR 时才填。
- **复制拓扑**（`LINUX_PORT_GAPS.md` §1.4）：一个 logical monitor 可含多个 connector。
  `is_topology_valid`/`set_topology` 在 Mutter 会话下放行多设备组并按组重建 logical monitor
  （已存在的组沿用原 x/y/scale/transform/primary，新组依次排到右边），`get_current_topology` 用
  `compositor_output_t::replication_source` 把同组输出聚合回一个镜像组，因此 `set_topology` 的
  回读校验对镜像拓扑同样成立。
- **已知约束**：`set_hdr_states`/`set_display_modes` 的"回读确认"用的是 `query_outputs()`，即
  每次重试都重新走一遍 D-Bus（Mutter 的 serial 只有一次有效，不能缓存）。
- **测试**：`tests/unit/test_mutter_display.cpp` 7 例，用真实 sd-bus 消息做编解码往返（构造
  `GetCurrentState` 的返回签名与 `ApplyMonitorsConfig` 的请求体后逐字段读回），不需要合成器；
  没有会话总线时整组 GTEST_SKIP。开发中被这批用例抓出 3 个真 bug：根 tuple 不能
  `enter_container`、monitor 结构签名少一层括号、缺可变 `find_monitor` 重载。
- **未做运行时验证**：开发机上没有 mutter/gnome-shell，`busctl --user introspect` 返回
  `The name is not activatable`。等有 GNOME 会话时可做端到端确认（切分辨率/HDR/主屏 + 镜像拓扑
  + 还原）。

### 6.19 KScreen 镜像拓扑

旧文档断言"kscreen-doctor 没有 replication 命令"，这条是错的：`kscreen-doctor --help` 里就有
`output.HDMI-2.mirror.eDP-1` / `output.HDMI-2.mirror.none`。于是 §1.4 的 KDE 半边不需要 KWin 脚本，
也不需要 libkscreen D-Bus。

- **写入**：`set_topology` 在使能/禁用输出之后追加一次 `kscreen_change`，参数由纯函数
  `src/platform/linux/mirror_plan.h` 的 `platf::mirror::plan()` 算出 —— 它只发差量：
  成员指向组首（`mirror.<source>`），不再需要镜像的输出显式 `mirror.none`（不清就会留下残留关系），
  已经正确的组合一条都不发。
- **能力判定**：`compositor_supports_replication()` 对 Mutter/KScreen 返回 true，对 niri 返回
  false —— niri 没有镜像概念，多设备组在校验层就被拒绝（早失败，报错信息与 setter 一致）。
- **回读**：`get_current_topology()` 用 `compositor_output_t::replication_source`（KScreen 与
  Mutter 都填"源输出的数字 id"）把镜像成员折叠回一个组，因此三后端的拓扑比较、`set_topology`
  自动回退、`apply_config` 的还原校验共用同一套逻辑。
- **顺序**：镜像必须在使能之后发（禁用的输出不能当镜像目标）；失败时回退路径同样重放
  `apply_kscreen_mirroring(current)`，best effort。
- **实测（本机 KDE，2026-10-22）**：`kscreen-doctor output.eDP-1.enable output.eDP-1.mirror.DP-2`
  之后 `kscreen-doctor -o` 报 `eDP-1 enabled` + `replication source: 1`（1 = DP-2 的输出 id），
  随后 `output.eDP-1.disable` 复原。这条命令正是 `mirror_plan` 生成的形态，回读字段也正是
  `query_kscreen_outputs()` 解析的 `replication source:`。
- **实测发现的怪癖**：输出被禁用后**会保留**残留的 `replication source`（eDP-1 关掉后仍报
  `replication source: 1`）。所以 `mirror_plan` 不碰禁用输出上的关系（动它会让整条原子请求失败），
  而是靠"使能 → 再回收"的顺序兜住：需要独立的输出先被使能（此时旧关系复活），随后
  `apply_kscreen_mirroring()` 发现它已启用且不在期望成员里，补一条 `mirror.none` 清掉。
  这条顺序依赖就是 `tests/unit/test_mirror_plan.cpp` 里 `IgnoresAMirrorOnADisabledOutput` 与
  `SwapsWhichOutputCarriesTheRelation` 两个用例的来源。
- **测试**：`tests/unit/test_mirror_plan.cpp` 8 例 —— 已镜像不重复发、新镜像、回收、改指向、交换
  方向（先回收再重指，两条命令的顺序也固定）、禁用输出上的残留关系不碰、未知输出拒绝、扩展桌面
  不变。

### 6.20 HDR 亮度分析：逐像素 max(R,G,B)

- **问题**：Windows 分析器是 D3D11 计算着色器跑 scRGB 帧，看到的是真 max(R,G,B)；Linux 走 KMS
  捕获的 PQ YUV，旧实现直接拿**亮度**当 maxRGB 累加。亮度恒 ≤ maxRGB，饱和纯色下差距是量级级别：
  纯红 100 nits 的帧，亮度只有约 1.2 nits。
- **做法**：`src/video_hdr_analyzer.h`（纯 header，`namespace video::hdr_analyzer`）把每个
  Y'CbCr 三元组按非恒定亮度反矩阵还原成 R'G'B' 的 PQ 信号，取最大者落回 10-bit 码进直方图。
  矩阵系数按帧的 `colorspace` 选：BT.2020 NCL 默认，只有显式 BT.709/470BG/170M/240M 才换 BT.709
  系数（否则 SDR 帧会被 2020 系数算歪）。`G' = Y' - (Kr·R' + Kb·B')/Kg` 用 `Kg = 1 - Kr - Kb`
  归一，灰色三元组因此精确回到自身亮度——这是 `GreyKeepsItsLuma` 用例守的等式。
- **色度取址**：4:4:4 直接同址；4:2:0（P010LE / YUV420P10LE）按最近邻取 `col/2, row/2`。分析在
  已降采样到 ≤1920×1080 的帧上跑，插值成本换不来精度。
- **为什么不上 GPU**：`hdr_analysis_interval = 4` ⇒ 每 4 帧一次、帧已降采样，CPU 每像素 6 次乘加
  可以忽略；而 CUDA/Vulkan 路径要给 scRGB hwdevice 与 VAAPI/软件两条链路都加分支。PQ 数学仍只由
  `video_hdr_metadata.h` 提供（`pq_to_nits` / `st2084_*`），没有第二份实现。
- **契约修正**：`platf::hdr_frame_luminance_stats_t` 的字段注释本来就写着 max(R,G,B)
  （`src/platform/common.h:527-549`），旧实现与之不符；现在实现与文档一致，Windows 软件路径同样
  受益（硬件路径本来就走着色器）。
- **测试**：`tests/unit/test_video_hdr_analyzer.cpp` 10 例 —— 纯红给 100 nits 而不是 1.2、
  灰阶等式、有限/全范围等价、MSB/LSB 等价、4:2:0 色度覆盖每个像素、无色度回退亮度、黑帧
  near-black = 1、坏帧不发布零值、序列号逐帧递增、BT.709 独立矩阵。

---

## 七、功能可用性矩阵

### ✅ 完整可用

| 功能 | 说明 |
|---|---|
| 核心串流 / Moonlight 配对 / WebUI | 上游成熟代码，全平台 |
| 捕获：KMS / Wayland(wlr) / X11 | 见 §五第 4 条的合成器限制 |
| 编码：NVENC / VAAPI / Vulkan / 软件 | Linux 注册四个编码器；NVENC 走 FFmpeg avcodec + CUDA hwdevice（运行时加载驱动）；NVENC 的 YUV 4:4:4 已解禁（§6.10） |
| 输入注入 + 手柄振动 | inputtino（libevdev/uinput），含 rumble 回放 |
| 触觉反馈（haptics 协议层） | DS5 PCM 分析 → IR v2 回发客户端、legacy rumble 合成；仅"主机直连 DualSense 播放"的 sidecar 是 Windows 专属 |
| 文本输入通道 text_context | 纯数据通道 |
| 文件夹共享 file_mapping | HTTP/WS/RPC/token 全套跨平台；"资源管理器右键共享"入口是 Windows 专属 |
| Webhook / client_fingerprint / launch_session_manager / ABR | 纯逻辑层；ABR 前台检测已移植（KDE + niri，§6.6） |
| 虚拟显示器 | 原生 EDID 后端 + 全组合模式表 + prep 模式 + 独占还原 + 残留 adopt（§6.1） |
| 物理显示器 display_device | kscreen/niri 分辨率 + 拓扑开关 + 持久化还原；HDR/主屏仅 kscreen（§6.2、§6.16） |
| 剪贴板主机同步（文本 + 图片） | KDE：文本走 klipper sd-bus、图片走 ext-data-control；无 klipper 的桌面（niri/sway/Hyprland）文本也走 ext-data-control。共用帧/回声/blob 回退（§6.4、§6.14） |
| 远程麦克风写主机 | 虚拟 null-sink + 背压队列 + 默认录音设备切换（§6.5） |
| HDR 静态元数据透传（MDCV/CLL） | kmsgrab 读 DRM blob → avcodec side data |
| HDR10+ / DV P8.1 / HDR Vivid（动态） | avcodec 路径全通（§6.3），分析器为逐像素 maxRGB（§6.20） |
| 增强托盘 | 打开 UI/语言/项目链接/重启/退出 + 通知 + VDD 子菜单 + Advanced Settings（§6.9） |
| nvhttp 扩展 API | dynamic_params / network_probe / sessions / abr_api / ai_api / pairing，纯 HTTP 层 |
| perf_recorder / input_activity / video_probe / cursor_channel | 跨平台；cursor_channel 在 Linux 因无光标生产者干净禁用 |
| 音频增强：Opus DRED / 持续音频 / 7.1.4 12 声道 | DRED 是 libopus≥1.5 编译期特性检测；12ch 有 Linux null-sink 实现 |
| AI API Key 凭据 | Secret Service（libsecret）持久化，与 Windows 的 DPAPI 对等；无 keyring 时降级为环境变量 `SUNSHINE_LLM_API_KEY`（§6.11） |
| DSU 运动广播（cemuhook） | 共享协议实现 + inputtino motion 转发，Cemu/Ryujinx 可用；需客户端用手柄串流并开启 `enable_dsu_server`（§6.17） |

### ⚠️ 降级可用（编译运行正常，功能面收窄）

| 功能 | Linux 状态 |
|---|---|
| 远程 USB（USB/IP 主机） | 后端已实现（usbip-utils 方言，见 §6.15）；vhci `attach` 属性仅 root 可写，需 root 运行或 sudoers 包装才能端到端启用 |
| HLG 编码与 DV P8.4 | kmsgrab 不接受 HLG EOTF（`kmsgrab.cpp:951-952`），属内核 UAPI 限制（`HDR_OUTPUT_METADATA` 仅定义 PQ/SDR）；P8.1 已覆盖 DV 主流 |
| 非 niri 的 wlroots / X11 显示后端 | niri 已由 IPC 后端覆盖（§6.16），GNOME 由 Mutter D-Bus 覆盖（§6.18）；sway/Hyprland 等仍走"合成器不可用"降级并打 warning（见 `LINUX_PORT_GAPS.md` §2.10） |
| display_device 复制拓扑 | 两个平台都可用：Mutter 写成同一个 logical monitor（§6.18），KScreen 用 `output.<name>.mirror.<source>`（§6.19）；回读统一经 `replication_source` 聚合（见 `LINUX_PORT_GAPS.md` §1.4） |
| frame_contract（帧管线契约） | 策略层跨平台（`src/platform/frame_contract.cpp`），但只有 Windows 采集端消费 |
| 虚拟显示器 per-client 隔离 | 客户端切换时销毁重建 VDD，单虚拟屏共享；Windows 是多实例 + 每客户端独立 GUID |

### ❌ Windows 专属 / 不可移植

| 功能 | 原因 |
|---|---|
| NVENC SDK 13 直连 / AMF QVBR / 多硬件实例 | `src/nvenc/`、`src/amf/` 仅进 Windows 构建；Linux NVENC 为 FFmpeg avcodec 路径 |
| 虚拟扬声器位深匹配 | Windows PolicyConfig COM；Linux 固定 `PA_SAMPLE_FLOAT32`（PipeWire 服务端自适应，质量无损） |
| 触摸键盘自动唤起 | Windows 注册表机制，头文件自述非 Windows 即 no-op |
| Tauri 控制面板 | 分发链 Windows 化；约 20 个系统模块仅 Windows 实现 |
| vmouse 虚拟鼠标 / WGC 捕获 / rtx_hdr / pre_encode_filter / DS5 sidecar | 全在 `src/platform/windows/` |
| Zako Direct 零拷贝借帧 | D3D11 共享纹理，Linux 无对等机制（KMS 捕获直读 DRM plane 已覆盖功能面） |

不可移植项的完整原因与 Linux 对应物见 `LINUX_PORT_GAPS.md` §三。

---

## 八、编码器与本机 GPU（RTX 3050 Mobile, Ampere sm_86）

- **首选 `nvenc`**（h264_nvenc / hevc_nvenc / av1_nvenc，经 FFmpeg）——不需要 `cuda` 包。
- **YUV 4:4:4 已解禁**：客户端开 4:4:4 后 h264 444=8-bit、hevc 444=10-bit HDR；Ampere 上
  h264/hevc 444 可用，AV1 444 需 Ada（探测自动拒绝）。
- **CUDA 默认 ON**：CUDA 加 NVFBC 路径并移除 NVENC 的 GPU→RAM→GPU 往返；普通 KMS/Wayland/X11
  路径仍在编译内。CUDA 13.4 / sm_86 实测编译+链接通过。**运行时未在本沙箱验证**（无 GPU）；
  日志里 `Attempting to use NVENC without CUDA support. Reverting back to GPU -> RAM -> GPU`
  应消失。
- **`vulkan`** 编码器是该分支跨平台新增项，可作实验选项；**`vaapi`** 在 NVIDIA 上不适用。

**HDR 全链路边界**：**可用** = PQ 静态元数据透传、HDR10+、DV P8.1、HDR Vivid（均见 §6.3）；
**不可用** = HLG（kmsgrab 不支持 HLG EOTF 输入）、DV P8.4（需 HLG 基层）。

---

## 九、测试基线

2026-10-22（并入上游 `7a0995e2` 之后），CUDA=ON 构建树：

```
ctest: 24 个套件，23 通过，1 失败（test_sunshine）
test_sunshine: 733 用例，719 通过，12 跳过，2 失败
```

两个失败都是上游新增用例的**环境依赖**，不是移植回归（详见 `LINUX_PORT_GAPS.md` §7.2 / §7.3）：

- `DownloadFileTests/DownloadFileTest.Run/1` 访问 `https://httpbin.org/...`——离线必失败。
- `NvhttpApps.StreamsLargeAssetOverTlsAndPreservesNotFoundAndBadArguments` 期望未知 appid 返回
  404，但 `DEFAULT_APP_IMAGE_PATH` 编译期就是 `/usr/share/sunshine/box.png`，本机装过包所以该
  文件存在 → 返回 200。用 `unshare -rm` 把空目录 bind-mount 到 `/usr/share/sunshine` 后该用例通过。

三个 `SetUpTestSuite` 失败（`Configurations/AudioTest`、`MouseInputs/MouseHIDTest`、
`EncoderVariants/EncoderTest`）仍是无头环境预期行为。

**注意事项**：
- 直跑 `ctest` 需要可写的 `$HOME`（FileHandler 用例在 `~/.config/sunshine` 下建目录），否则聚合套件
  提前 abort。
- 增量只编 `sunshine` 时 `ctest` 会跑旧的 `test_sunshine` 并掩盖新失败——改测试相关代码后要重建该目标。

---

## 十、标签与提交约定

**里程碑标签阶梯**（每完成一个功能里程碑继续延伸）：

`v0.1-linux-base` → `v0.2-linux-vdd` → `v0.3-linux-display-device` → `v0.4-linux-clipboard-host` →
`v0.5-linux-mic-redirect` → `v0.5.1-linux-restart-fixes` / `v0.5.2-linux-tray-restart` →
`v0.6-linux-abr-foreground` → `v0.7-linux-hdr10plus` → `v0.8-linux-dv-rpu` →
`v0.8.1-linux-tray-vdd-state` / `v0.8.2-linux-tray-msgbox` →
`v0.8.3-linux-manual-vdd` / `v0.8.4-linux-vdd-mode-list` →
`v0.8.5-linux-vdd-name` / `v0.8.6-linux-parity-audit` → `v0.8.7-linux-vdd-refresh-ladder` →
`v0.9.0-linux-full-mode-table` → `v0.9.1-linux-full-cross-product` → `v0.9.2-linux-nvenc-yuv444` →
`v0.9.3-linux-upstream-sync`（**当前**）

**提交约定**：每个逻辑步骤一个 commit；commit message 用英文、conventional-commit 风格
（`feat(linux): …`）。用户读中文。

**Windows 中立性**：本仓库由 Windows CI 构建，任何共享代码改动都必须保证 Windows 行为恒等或在
`#ifdef _WIN32` / `#if !defined(_WIN32)` 门控内。落地后用
`git diff <merge-base>..HEAD -- <shared file>` 审计。详见 `AGENTS.md` 的 Platform rules 与
Windows parity conventions。
