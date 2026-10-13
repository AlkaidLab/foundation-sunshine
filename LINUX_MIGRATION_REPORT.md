# Foundation Sunshine Linux（Arch Linux）迁移报告

- **基线**：`linux-migration` 分支，tag `v0.9.3-linux-upstream-sync`（pkgrel 65，2026-10-12，
  HEAD `449021c7`；已并入上游 `origin/master` `ed415026`）
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

已移植项的清单与证据见 §六；剩余缺口（含 7 项部分实现、可行未做项、本质不可移植项）
在 `LINUX_PORT_GAPS.md` 逐条编号维护。

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
  libpulse libevdev miniupnpc opus curl openssl
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

### 6.2 物理显示器 display_device（kscreen 后端）

`src/platform/linux/display_device.cpp`（1703 行）以 kscreen-doctor 为合成器中介读写模式/HDR/主屏/
拓扑，JSON 持久化 + apply/revert 编排对齐 Windows `settings.cpp`。

- `:143` `query_outputs()`、`:706` `set_display_modes()`、`:913` `set_hdr_states()`、
  `:1035` `set_topology()`、`:360` `wait_for_topology()`、`:383` `wait_for_display_stability()`。
- 模式匹配用 Windows 的 1 Hz 模糊比较 + 取最近候选；失败时按三段式回滚而非留下半套模式。
- 空 device_id（Display: Auto）解析为主屏（kscreen priority 1），模式/HDR 不再作用于所有输出。
- 拓扑校验与 setter 一致地拒绝多设备组（复制组当前不可表达，见 `LINUX_PORT_GAPS.md`）。
- kscreen 不可用的环境退化为既有的 no-op 行为并打 **warning**（明确"显示设置未被应用"），
  但保留 success 结果——这是 Linux 上会话的持久属性，返回失败会让所有非 KDE 会话进入无法修复的
  deferred-retry。

### 6.3 HDR 动态元数据三件套（avcodec 路径）

静态透传本就跨平台：`src/platform/linux/kmsgrab.cpp:864` 读 DRM `HDR_OUTPUT_METADATA` blob、
`:930` 解析，`src/video.cpp:3443` 挂 mastering display side data、`:3462` 挂 content light。

动态部分由 CPU 亮度分析器供给统计：

- **分析器**：`src/video.cpp:396` `analyze_pq_luma_frame()`（限幅范围重映射的直方图，
  PQ→nits 复用 `hdr_metadata::pq_to_nits`）；软件设备路径 `:567`，硬件设备采样下载路径 `:1036`。
  平面 10-bit 格式按格式掩码解包（P010/YUV444P16 为 MSB，YUV420P10LE/YUV444P10LE 为 LSB）。
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

### 6.4 剪贴板主机同步（klipper D-Bus）

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
- **边界对齐 agent**：出站过滤空读取；入站丢弃空串与含 NUL 文本。**文本类**——Linux 不通告
  `clipboard_image`（provider 仅文本）；大文本走 blob store 的 `kKindRef` 描述符帧。

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
| 物理显示器 display_device | kscreen 分辨率/HDR/主屏/拓扑 + 持久化还原（§6.2） |
| 剪贴板主机同步（文本类） | klipper sd-bus 双向 + 回声抑制 + blob 回退（§6.4） |
| 远程麦克风写主机 | 虚拟 null-sink + 背压队列 + 默认录音设备切换（§6.5） |
| HDR 静态元数据透传（MDCV/CLL） | kmsgrab 读 DRM blob → avcodec side data |
| HDR10+ / DV P8.1 / HDR Vivid（动态） | avcodec 路径全通（§6.3） |
| 增强托盘 | 打开 UI/语言/项目链接/重启/退出 + 通知 + VDD 子菜单 + Advanced Settings（§6.9） |
| nvhttp 扩展 API | dynamic_params / network_probe / sessions / abr_api / ai_api / pairing，纯 HTTP 层 |
| perf_recorder / input_activity / video_probe / cursor_channel | 跨平台；cursor_channel 在 Linux 因无光标生产者干净禁用 |
| 音频增强：Opus DRED / 持续音频 / 7.1.4 12 声道 | DRED 是 libopus≥1.5 编译期特性检测；12ch 有 Linux null-sink 实现 |
| AI API Key 凭据 | Linux 降级为明文环境变量 `SUNSHINE_LLM_API_KEY`（Windows 用 DPAPI，见 `LINUX_PORT_GAPS.md` §1.6） |

### ⚠️ 降级可用（编译运行正常，功能面收窄）

| 功能 | Linux 状态 |
|---|---|
| HLG 编码与 DV P8.4 | kmsgrab 不接受 HLG EOTF（`kmsgrab.cpp:951-952`），属内核 UAPI 限制（`HDR_OUTPUT_METADATA` 仅定义 PQ/SDR）；P8.1 已覆盖 DV 主流 |
| 非 KDE 显示后端 | display_device 的模式/HDR/拓扑目前只有 kscreen 实现；niri/wlroots/X11 走"合成器不可用"降级并打 warning（见 `LINUX_PORT_GAPS.md` §2.10） |
| display_device 复制拓扑 | kscreen-doctor 无 replication 设置命令，多设备组被拒绝（见 `LINUX_PORT_GAPS.md` §1.4） |
| frame_contract（帧管线契约） | 策略层跨平台（`src/platform/frame_contract.cpp`），但只有 Windows 采集端消费 |
| HDR 分析精度 | CPU 亮度直方图（亮度域）vs Windows 的 D3D11 逐像素 maxRGB（scRGB）；消费端按近似处理 |
| 虚拟显示器 per-client 隔离 | 客户端切换时销毁重建 VDD，单虚拟屏共享；Windows 是多实例 + 每客户端独立 GUID |

### ❌ Windows 专属 / 不可移植

| 功能 | 原因 |
|---|---|
| NVENC SDK 13 直连 / AMF QVBR / 多硬件实例 | `src/nvenc/`、`src/amf/` 仅进 Windows 构建；Linux NVENC 为 FFmpeg avcodec 路径 |
| 虚拟扬声器位深匹配 | Windows PolicyConfig COM；Linux 固定 `PA_SAMPLE_FLOAT32`（PipeWire 服务端自适应，质量无损） |
| 触摸键盘自动唤起 | Windows 注册表机制，头文件自述非 Windows 即 no-op |
| Tauri 控制面板 | 分发链 Windows 化；约 20 个系统模块仅 Windows 实现 |
| USB/IP 主机（remote_usb） | 非 Windows 显式返回 `unsupported`（可行，见 `LINUX_PORT_GAPS.md` §2.2） |
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

2026-10-13，HEAD `449021c7`，CUDA=ON 构建树：

```
ctest: 15 个套件，14 通过，1 失败（test_sunshine）
test_sunshine: 640 用例 / 95 套件，628 通过，12 跳过，0 断言失败
```

唯一失败是三个需要真实音频/输入/编码器环境的套件的 `SetUpTestSuite`
（`Configurations/AudioTest`、`MouseInputs/MouseHIDTest`、`EncoderVariants/EncoderTest`），
在无头环境下预期失败。

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
