# Foundation Sunshine Linux（Arch Linux）迁移报告

- **日期**：2026-09-08（**更新**：2026-09-11 —— 虚拟显示器模式表补全为配置全组合（链式 CTA 块，tag
  `v0.9.1`，pkgrel 38）；HDR10+ 与 DV P8.1 在 avcodec 路径接通；托盘 VDD 状态/消息框落地，进度见 §十二）
- **对象仓库**：`foundation-sunshine`（LizardByte/Sunshine 的 AlkaidLab 增强分支，commit `3e142f7`）
- **目标环境**：Arch Linux / CachyOS（x86_64_v3，KDE Plasma + Wayland + PipeWire，NVIDIA RTX 3050 Mobile）

---

## 一、总体结论

**可以迁移，但不是开箱即用。** 代码层面该分支完整保留了上游 Sunshine 的 Linux 平台层
（`src/platform/linux/`、`cmake/compile_definitions/linux.cmake`、`docker/archlinux.dockerfile`、
`third-party/build-deps/dist/Linux-x86_64` 预编译 FFmpeg），核心串流能力（KMS/Wayland/X11 捕获、
NVENC/VAAPI/软件编码、evdev 输入、WebUI）是上游成熟代码。但工程链有 **1 个硬阻塞**：

> **该分支裁掉了全部 Linux 打包模板**（`packaging/linux/` 下只剩 flatpak 的第三方依赖子模块），
> 而 CMake 在 UNIX 分支会无条件 `configure_file()` 这些文件 —— **任何 Linux 配置路径（原生 cmake、
> PKGBUILD、docker、flatpak）都会直接报错退出**。迁移第一步必须从上游恢复这批模板（见 §三）。

此外要建立正确的预期：迁移开始时本分支的卖点（ZakoVDD 虚拟显示器、NVENC SDK 13 直连 / AMF QVBR、
Tauri 控制面板、远程麦克风写主机、USB/IP 主机、WGC 捕获、vmouse 虚拟鼠标）在 Linux 上均不可用
（Windows 专属或不完整 stub）。**截至 2026-09-11 已移植**：ZakoVDD 虚拟显示器（原生 C++ 后端 +
配置驱动的全组合模式表，§十二进度 7/19–21）、物理显示器 display_device 后端（进度 11）、主机侧
剪贴板同步（进度 12）、远程麦克风写主机（进度 13）、ABR 前台检测（进度 15）、HDR10+ 亮度分析器
（进度 16）、DV P8.1 RPU（avcodec 路径，进度 17）、HDR Vivid（avcodec 路径，进度 24）、托盘 VDD
状态与消息框（进度 18）；其余项仍
不可用或降级，逐项清单见 §八功能矩阵与 `LINUX_PORT_GAPS.md`。CI 目前只有 Windows 构建
（`.github/workflows/main.yml` 仅有 `build_win`、`vdd_smoke` 两个 Windows job），Linux 编译在
主干上长期无人验证，首次构建可能遇到零星的编译错误。

---

## 二、平台支持现状速览

| 层面 | 现状 |
|---|---|
| Linux 平台代码 | `src/platform/linux/` 文件齐全（kmsgrab / wlgrab / x11grab / vaapi / cuda / wayland / graphics / audio / input），为上游成熟代码，无致命 stub |
| FFmpeg 依赖 | 不用系统 FFmpeg，用子模块 `third-party/build-deps/dist/Linux-x86_64` 的静态库（avcodec/avutil/swscale + x264/x265/SVT-AV1/hdr10plus/cbs），Linux 版存在 ✅ |
| Boost | 要求 **精确 1.92.0**；CachyOS 仓库当前就是 1.92.0 ✅；版本不匹配时 CMake 自动 FetchContent 联网下载（`cmake/dependencies/Boost_Sunshine.cmake`） |
| WebUI | Vue/Vite，`npm run build` 交叉生成静态资产，跨平台 ✅ |
| 托盘图标 | 需要 `libayatana-appindicator` + `libnotify`；且 `SUNSHINE_REQUIRE_TRAY` **默认 ON**，缺失时直接 FATAL（`cmake/compile_definitions/linux.cmake:206`） |
| CUDA | 实际可选（`CUDA_FAIL_ON_MISSING` 选项声明了但从未被使用）；CUDA 工具包只编译 `cuda.cu`（NvFBC/CUDA 捕获路径） |
| CI | **无任何 Linux 构建/测试 job**（`.github/workflows/main.yml`） |
| 打包模板 | **初始全部缺失**（见 §三）；**已修复**——模板自上游恢复，`packaging/arch-local/` 为本地打包入口（§十二进度 1、10） |

---

## 三、硬阻塞：Linux 打包模板缺失（必须先修）—— ✅ 已修复（2026-09-09）

> **状态**：该阻塞已解决——上游旧布局的模板已拷回并提交（`1b15942`，§十二进度 1），
> 后续打包走 `packaging/arch-local/`（pkgrel 38）。以下保留问题记录与当时的实测输出备查。

`cmake/prep/special_package_configuration.cmake` 在 UNIX 下引用的以下文件**均不存在于仓库**
（`packaging/linux/` 下实际只剩 `flatpak/deps/` 两个子模块）：

| 缺失文件 | 引用位置 | 影响的构建路径 |
|---|---|---|
| `packaging/linux/sunshine.desktop` | special_package_configuration.cmake:24 | 原生构建（默认路径） |
| `packaging/linux/sunshine_terminal.desktop` | :25 | 原生构建 |
| `packaging/linux/sunshine.appdata.xml` | :29 | 原生构建（无条件） |
| `packaging/linux/sunshine.service.in` | :32 | 原生构建（无条件，systemd 用户服务） |
| `packaging/linux/Arch/PKGBUILD` + `Arch/sunshine.install` | :36-37 | AUR/makepkg（`docker/archlinux.dockerfile` 也走这条路） |
| `packaging/linux/AppImage/sunshine.desktop` | :15 | AppImage |
| `packaging/linux/flatpak/sunshine.desktop`、`sunshine_kms.desktop`、`<FQDN>.metainfo.xml`、`<FQDN>.yml`、`modules/` | :18-22, 42-44 | Flatpak |

`configure_file()` 对不存在的输入文件会直接报 fatal error，因此 **Linux 下 `cmake -B build` 第一步就会失败**。

**修复方案**：从上游 LizardByte/Sunshine master 拷回上述文件即可（它们是纯模板，用 `@VAR@` 占位，
与分支的 CMake 变量兼容——分支对这些 CMake 逻辑没有改动）。以 Arch 为目标时最少需要恢复：
`sunshine.desktop`、`sunshine_terminal.desktop`、`sunshine.appdata.xml`、`sunshine.service.in` 四个文件；
若想走 makepkg 路线再补 `Arch/PKGBUILD` + `Arch/sunshine.install`。

**实测记录（2026-09-09，本机）**：`cmake -B build` 确认精确输出上述 4 条 configure_file 错误。
CMake 配置阶段错误会**累积而非首条中断**，最终以 `Configuring incomplete` 收场；同一次配置还暴露
了第二类问题——多个子模块工作树处于空置状态（见 §十 风险 9）。除此之外全部系统依赖检测一次性
通过：Boost 1.92 系统版直连（未触发 FetchContent）、OpenSSL 3.6.4、libdrm/libcap、libva 1.24、
Wayland + wayland-scanner、X11、ayatana-appindicator 0.6.0 + libnotify（托盘未触发 FATAL）、
CUDA 缺失自动跳过（实证其确实可选）、DualSense 走 uhid 实现。

---

## 四、Arch Linux 依赖清单

### 构建依赖（pacman 包名）

| 包 | 用途 | 本机（CachyOS）状态 |
|---|---|---|
| `base-devel` | 编译工具链（本机 gcc 16.2.1） | ✅ 已有工具链 |
| `cmake` (≥3.25) | 构建系统 | ✅ 4.4.3 已装（2026-09-09 实测配置通过依赖检测） |
| `ninja` | 生成器 | ✅ 1.13.2 已装 |
| `git` | 子模块 | ✅ |
| `boost` 1.92.0 | 精确版本要求 | ✅ 1.92.0 已装（系统版直连，未触发 FetchContent） |
| `openssl` / `curl` / `miniupnpc` / `opus` | 网络与音频编解码 | ✅ 均已装 |
| `libdrm` + `libcap` | KMS 捕获 | ✅ 已有 libdrm；libcap 已装 |
| `libva` | VAAPI 编码 | ✅ |
| `wayland` + `wayland-protocols` | Wayland 捕获（protocols 默认用子模块，或 `-DSUNSHINE_SYSTEM_WAYLAND_PROTOCOLS=ON`） | ✅ |
| `libx11` 及 xcb/xrandr/xtst/fixes/xi/xinerama | X11 捕获 | ✅ |
| `libpulse` | 音频捕获/播放（PipeWire 的 pulse 兼容层） | ✅ |
| `libevdev` | 输入注入（inputtino） | ✅ |
| `numactl` | 链接静态 FFmpeg 需要 libnuma | ✅ 2.0.19 已装 |
| `libayatana-appindicator` + `libnotify` | 托盘（`SUNSHINE_REQUIRE_TRAY` 默认 ON） | ✅ 0.6.0 + 0.8.8 已装（配置实测 FOUND） |
| `node` ≥26.7 且 `<27`、`npm` ≥11.19 且 `<12` | WebUI 构建（`package.json` engines 硬约束） | ⚠️ node 26.8.1 ✅，**npm 仍缺失**。注：仓库无 `.npmrc` engine-strict，Arch 源 npm 12.0.2 只会触发 EBADENGINE **警告**而非失败，可直接 `sudo pacman -S npm`；要严格对齐再用 fnm 按 `.node-version`（26.7.0）装。**不可用 pnpm 替代**：CMake 硬编码 `find_program(NPM npm REQUIRED)`（`targets/common.cmake:58`）；`packageManager: "npm@11.19.0"` 会触发 pnpm 9+ 严格模式拒绝；仓库只有 package-lock.json，pnpm 不读它，依赖解析会漂移 |
| `cuda`（可选） | 仅编译 NvFBC/CUDA 捕获 | ❌ 未装；**建议不装**（见 §九：仓库 cuda 13.3 已删除旧架构，而 CMake 自动推导的架构表含 sm_50–72，nvcc 会报错；GeForce 上 NvFBC 本就不可用） |
| `nlohmann-json`（可选） | `-DSUNSHINE_SYSTEM_NLOHMANN_JSON=ON` 时替代子模块 | — |

### 运行时依赖

上表库的运行时包 + `nvidia-utils`（本机 610.57.04 ✅，NVENC 无需额外 SDK）+ `pipewire`（✅，经
pulse API 供音频）。安装后按 §七做 `setcap`/udev 配置。

### 一键安装（构建依赖）

```bash
sudo pacman -S --needed cmake ninja boost numactl \
  libayatana-appindicator libnotify \
  libdrm libcap libva wayland libx11 libxcb libxrandr libxtst libxfixes libxi libxinerama \
  libpulse libevdev miniupnpc opus curl openssl
# node/npm 建议走版本管理器（engines 要求 npm 11.x）：
# fnm use 26.7.0  或  npm i -g npm@11.19.0（在已有 node 26 环境下）
```

---

## 五、推荐构建流程（修复模板之后）

```bash
# 0) 先恢复 §三 所列模板（从上游 LizardByte/Sunshine 拷贝）

# 1) 子模块（⚠️ 本机曾出现多个子模块工作树被清空的损坏状态，--force 可强制恢复固定提交）
git submodule update --init --recursive --force

# 2) 配置（参数对齐 scripts/linux_build.sh 的 Linux 形态）
cmake -B build -G Ninja -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/sunshine \
  -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine \
  -DSUNSHINE_ENABLE_CUDA=OFF          # RTX 3050 不需要 NvFBC/CUDA 捕获；NVENC 走 FFmpeg 不受影响

# 3) 编译（WebUI 会在构建期内自动 npm install + build）
ninja -C build

# 4) （可选）先跑测试验证移植质量：BUILD_TESTS=ON 时 test_sunshine 目标
#    会把全部跨平台源码编译进一个 gtest 可执行文件，等于一次完整的 Linux 编译验证
ninja -C build test_sunshine && ./build/test_sunshine   # 或 ctest --test-dir build

# 5) 安装
sudo ninja -C build install
```

安装形态对比：

| 路线 | 可行性 | 说明 |
|---|---|---|
| `ninja install` 直装 | ✅ 推荐（最快验证） | 卸载需手动；适合先跑通 |
| PKGBUILD / makepkg | ⚠️ 需恢复 `Arch/PKGBUILD` 模板 | 上游模板即按 build-deps 静态 FFmpeg 设计；生成 pacman 包便于卸载/升级，是长期方案 |
| `docker/archlinux.dockerfile` | ⚠️ 同样依赖被裁掉的模板 | 模板恢复后可直接用 |
| Flatpak | ❌ 短期不建议 | manifest 本体也缺失，且分支改动大，适配成本高 |

---

## 六、打包产物在 Linux 上包含什么

`cmake/packaging/linux.cmake`：安装 udev 规则（`60-sunshine.rules`）、systemd 用户服务
（`sunshine.service`）、桌面/终端入口、图标与 Linux 资产（`src_assets/linux/assets/` 含
apps.json 与 GLSL shaders）。deb/rpm 的 postinst 会执行
`setcap cap_sys_admin+p $(which sunshine)` 并触发 udev 重载——**Arch 直装路径没有这一步，必须按 §七手动做**。

---

## 七、运行时配置（KDE Wayland + NVIDIA 场景）

1. **KMS 捕获权限**（Wayland 下实际唯一可用的整屏捕获路径，见下）：
   ```bash
   sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))
   ```
2. **输入设备权限**：随包安装的 `60-sunshine.rules` 给 `/dev/uinput`、`/dev/uhid` 打 `uaccess` 标签
   （直装时手动拷到 `/etc/udev/rules.d/` 并 `udevadm control --reload-rules`）。
3. **systemd 用户服务**：模板恢复后安装到 `~/.config/systemd/user/`（或系统路径），`systemctl --user enable --now sunshine`。
4. **捕获方式现实（KWin Wayland）**：KWin **不支持** wlr-export-dmabuf/wlr-screencopy 协议，
   所以 `wlgrab` 在 Plasma Wayland 上不可用；`x11grab` 只能抓到 XWayland 窗口；**实际可行的是
   `kmsgrab`（需第 1 步的 setcap）**，它按 DRM 输出抓帧（多显示器时选对输出）。要体验完整捕获矩阵
   可用 X11 会话或 wlroots 系合成器（Hyprland/Sway）。
5. **NVIDIA**：确认 `nvidia-drm.modeset=1`（CachyOS 默认已开）。NVENC 由 FFmpeg 运行时加载驱动，
   无需 CUDA Toolkit。
6. **端口**：TCP 47984–47990、48010；UDP 47998–48000。Arch 默认无防火墙；若启用了 nftables 需放行。
7. **音频**：PipeWire 经 `pipewire-pulse` 兼容 libpulse，正常工作。

---

## 八、分支功能在 Linux 下的可用性矩阵

依据：跨平台源文件统一进 `SUNSHINE_TARGET_FILES`（`cmake/compile_definitions/common.cmake:54-254`），
平台差异靠 `#ifdef _WIN32/#else` 处理；以下均经源码核实（含证据行号）。

### ✅ 完整可用

| 功能 | 说明 |
|---|---|
| 核心串流 / Moonlight 配对 / WebUI | 上游成熟代码，全平台 |
| 捕获：KMS / Wayland(wlr) / X11 | 见 §七第 4 条的合成器限制 |
| 编码：NVENC / VAAPI / Vulkan / 软件 | Linux 注册四个编码器（`src/video.cpp:1633-1648`）；NVENC 走 FFmpeg avcodec + CUDA hwdevice（运行时加载驱动） |
| 输入注入 + 手柄振动 | inputtino（libevdev/uinput），含 rumble 回放 |
| 触觉反馈（haptics 协议层） | DS5 PCM 分析 → IR v2 回发客户端、legacy rumble 合成（`src/stream.cpp:1463-1500`）；仅"在主机直连 DualSense 上播放"的 sidecar 是 Windows 专属 |
| 文本输入通道 text_context | 纯数据通道（`src/text_context/`，挂接 `src/stream.cpp`） |
| 文件夹共享 file_mapping | HTTP/WS/RPC/token 全套跨平台；但"资源管理器右键共享"入口是 Windows 专属，Linux 只能经 API/WebUI 操作 |
| Webhook / client_fingerprint / launch_session_manager / ABR | 纯逻辑层，均有 Linux 分支；**ABR 前台应用检测已移植**：KDE 用 KWin 脚本经 D-Bus 推送活动窗口到进程内 `org.sunshine.Abr` 服务（2026-09-10，`d504fa11`）；**niri 用 `niri msg --json focused-window` 轮询**（2026-09-11，§十二进度 25），后端选择 KWin 优先、niri 兜底；其余桌面（wlroots/X11）维持空结果降级（`LINUX_PORT_GAPS.md` §2.6） |
| 剪贴板（客户端↔客户端 / 客户端↔WebUI 中继） | 内存中继 + SSE 跨平台；**主机侧同步已移植（2026-09-10）**：`src/clipboard_host.cpp` 对话 KDE klipper（sd-bus），双向同步 + 回声抑制，复用 GUI 代理的文本帧协议（§十二进度 12）。**文本类**：Linux 通告 `clipboard_text` 而不再通告 `clipboard_image`（provider 仅文本，对齐审计 §五 P4）；图片/大文件与 blob 回退仍缺（`LINUX_PORT_GAPS.md` §1.4） |
| AI API Key 凭据 | Linux 降级为明文环境变量 `SUNSHINE_LLM_API_KEY`（Windows 用 DPAPI，`src/ai/credential_store.cpp:117-124`） |
| 音频增强：Opus DRED / 持续音频 / 7.1.4 12 声道 | DRED 是 libopus≥1.5 的编译期特性检测（`src/audio.cpp:398-403`，Arch opus 1.6.1 ✅）；12ch 有 Linux null-sink 实现（`platform/linux/audio.cpp:406-408`）；持续音频为平台无关逻辑 |
| HDR 静态元数据透传（MDCV/CLL） | 跨平台：kmsgrab 读 DRM `HDR_OUTPUT_METADATA`（`kmsgrab.cpp:850-862`）→ avcodec side data（`video.cpp:2839-2862`），不依赖编码器 SDK |
| 编码器探测缓存（README"260x"） | 位于 `video.cpp` 探测层的跨平台缓存（`video.cpp:4534-4537`）；注意实现是内存缓存，README 的"持久化"表述与代码不符 |
| 增强托盘（fork 新增，上游无 src/tray） | 打开 UI/语言/项目链接/重启/退出 + 通知，Linux 可用；VDD 子菜单（"Zako HDR"）已随虚拟屏解禁，勾选态在 Qt 托盘正确渲染（需 `checkbox=1`，Windows 只看 `checked`），状态每 2s 轮询刷新且能 adopt 进程外残留的虚拟屏，危险操作走 QMessageBox 确认（§十二进度 18） |
| nvhttp 扩展 API：dynamic_params / network_probe / sessions / abr_api / ai_api / pairing | 纯 HTTP 协议层，零平台守卫，跨平台 |
| perf_recorder / input_activity / video_probe / cursor_channel | 全部跨平台；cursor_channel 在 Linux 因无光标生产者而干净禁用（`stream.cpp:2270-2273` 拒绝并告警，不影响流） |

### ⚠️ 降级可用（编译运行正常，功能面收窄）

| 功能 | Linux 状态 |
|---|---|
| Dolby Vision P8.1 / P8.4 | **P8.1 已接通 avcodec 路径（2026-09-10，`24817d1a`）**：RPU 写入器是纯比特流层（`video_dolby_vision.h:2-21`，0 平台守卫），avcodec 会话现按 NVENC 同款门控 configure 注入器、按提交帧序 stage L1、输出包按 pts splice RPU NAL，L1 亮度输入由 Linux 分析器产出；P8.4 需 HLG 基层，随 HLG 源一起搁置（⛔ 表） |
| HDR10+ 动态元数据 | **已完成（2026-09-10，`e49ae499`）**：side data 预挂与时间域滤波管线本为跨平台，缺的统计生产端已由 CPU 亮度直方图补齐（`analyze_pq_luma_frame`，限幅范围重映射，PQ→nits 复用 `hdr_metadata::pq_to_nits`），门控 = PQ 会话 + `hdr_luminance_analysis` + 客户端协商 HDR10+；近似性见 §十一 P2。**2026-09-11 修**：平面 10-bit 格式原按 P010 的 `>>6` 解包会读成全黑（仍标记 valid），现按格式掩码并接受 `YUV420P10LE`（§十二进度 23） |
| display_control / display_scale API | **虚拟屏路径已通**（2026-09-10）：设备枚举走 sysfs 连接器、capability_version 真实上报、prep 模式全量适配、EDID 通告配置列表的全部可行分辨率×刷新率组合（2026-09-11，§十二进度 21）；物理显示器的分辨率/HDR/拓扑已实现（kscreen-doctor，见 ❌ 表已移植行）。**2026-09-11 修**：Display: Auto（空 device_id）现解析为主屏（kscreen priority 1），模式/HDR 不再作用于所有输出 |
| frame_contract（帧管线契约） | 策略层跨平台（`platform/frame_contract.cpp`），但只有 Windows 采集端消费，Linux 侧策略存在、执行为空 |

### ❌ Windows 专属 / Linux 不可用

| 功能 | 原因 |
|---|---|
| ~~ZakoVDD 虚拟显示器~~ → **✅ 已移植（2026-09-10，模式表 2026-09-11 补全）** | 原生 Linux 后端：个性化 EDID（debugfs override）+ 连接器状态强制 + pidfd 借 DRM master 做 CRTC 指派 + 独占模式物理屏还原；5 种 prep 模式全适配；EDID 经**链式 CTA 扩展块**通告配置列表的全部可行分辨率×刷新率组合，显示名对齐 Windows `ZAKO_NAME`（"Zako HDR"）。已知偏差：per-client GUID 未实现（单虚拟屏共享）。详见 §十一 P0、§十二进度 7/19–21 |
| NVENC SDK 13 直连 / AMF QVBR / 多硬件实例 | `src/nvenc/`、`src/amf/` 仅进 Windows 构建（`compile_definitions/windows.cmake:114-120`）；Linux NVENC 为上游同款 FFmpeg 路径（注：探测缓存是跨平台的，见 ✅ 表） |
| HLG 编码（Linux 侧） | 会话框架跨平台，但 kmsgrab 不支持 HLG EOTF 输入（`kmsgrab.cpp:840-841`）→ Linux 无原生 HLG 源 |
| ~~HDR Vivid 动态元数据~~ → **✅ 已移植（2026-09-11，仅 Linux avcodec 路径）** | FFmpeg 只有 CUVA 解析器、无序列化器（bundled `libavutil.a` 已核实），故在 `encode_avcodec()` 内用共享 `serialize_vivid_t35()` 构建 T.35、按 pts 拼到首个 VCL NAL 之前（与 DV RPU 共用一次扩容）。Windows 的 avcodec 家族仍显式标记为不能承载 Vivid、走 NVENC/AMF 直连，故该拼接只在 Linux 编译（§十二进度 24） |
| 虚拟扬声器位深匹配 | Windows PolicyConfig COM（`platform/windows/audio.cpp:1316-1319`）；Linux 固定 `PA_SAMPLE_FLOAT32`（`platform/linux/audio.cpp:81`） |
| 触摸键盘自动唤起（touch_keyboard_session） | Windows 注册表机制，头文件自述非 Windows 为 no-op（`touch_keyboard_session.h:11-12`） |
| ~~Linux 物理显示器分辨率/HDR/拓扑切换~~ → **✅ 已移植（2026-09-10）** | `src/platform/linux/display_device.cpp` 重写：kscreen-doctor（KDE/compositor）读写模式、HDR、主屏、拓扑 + JSON 持久化还原，对齐 Windows settings.cpp 的 apply/revert 流程；VDD 拓扑仍由会话 VDD 阶段控制；kscreen 不可用时退化为历史 no-op 行为（§十二进度 11） |
| Tauri 控制面板 | 分发链 Windows 化（`FetchGUI.cmake:24`）；面板本体评估见 §十一 P3 |
| ~~远程麦克风写主机~~ → **✅ 已移植（2026-09-10）** | 虚拟 null-sink 方案：`sink-sunshine-virtual-mic` 的 monitor 源供主机应用录音，`write_mic_pcm` 写入混音后的单声道 PCM，返回码契约与 Windows VB-Cable 路径一致（§十二进度 13） |
| USB/IP 主机（remote_usb） | 非 Windows 显式禁用：`remote_usb_host_controller.cpp:71-82` 返回 `unsupported` |
| vmouse 虚拟鼠标 / WGC 捕获 / rtx_hdr / pre_encode_filter / DS5 sidecar | 全在 `src/platform/windows/` |

### ⚠️ 已知上游遗留小缺口（不阻塞）

`is_sink_available()` 恒返 true（`audio.cpp:509`）、CUDA 单设备（`cuda.cpp:300`）、kmsgrab 光标缩放
TODO、DPMS 关屏未实现（`misc.cpp:320`）等，均为上游 Sunshine 既有状态。

---

## 九、编码器与本机 GPU（RTX 3050 Mobile, Ampere sm_86）

- **首选 `nvenc`**（h264_nvenc / hevc_nvenc / av1_nvenc，经 FFmpeg）——不需要安装 `cuda` 包。
  本机 `nvidia-utils 610.57.04` 满足驱动要求。
- **建议 `-DSUNSHINE_ENABLE_CUDA=OFF`**：仓库 `cuda` 包是 13.3.1，CUDA 13 已移除 sm_50–72 等旧架构，
  而 `cmake/compile_definitions/linux.cmake:22-69` 按编译器版本自动推导的架构表包含这些旧值，
  nvcc 大概率直接报错。CUDA 仅服务 NvFBC/CUDA 捕获（GeForce 硬件本就不支持 NvFBC），关闭零损失。
  若确需开启，把该文件的架构表收敛为 `86`。
- **`vulkan` 编码器**是该分支跨平台新增项，可作实验选项；**`vaapi`** 在 NVIDIA 上不适用（NVIDIA 的
  VAAPI 支持.Undef 驱动侧很弱）。
- 注意：分支 HDR 全链路在 Linux 上的真实边界（经逐项核实）：**可用** = PQ 静态元数据透传
  （kmsgrab 读 DRM blob → avcodec side data）、DV P8.1/P8.4 框架（无亮度输入时退化为 HDR10）；
  **降级** = HDR10+（仅占位 SEI，无亮度分析器刷新）；**不可用** = HLG（kmsgrab 不支持 HLG EOTF
  输入）、HDR Vivid（avcodec 路径无 T.35 序列化器）、逐帧亮度分析（D3D11 Compute Shader）。
  移植路径见 §十一 P2。

---

## 十、风险清单与缓解

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | Linux 打包模板缺失，configure 必失败 | 🔴 阻塞 | 从上游恢复 4–6 个模板文件（§三） |
| 2 | 主干无 Linux CI，大量 2025 年后新增的跨平台代码从未在 Linux 编译过 | 🟠 高 | 先 `-DBUILD_TESTS=ON` 编 `test_sunshine` 全量验证；预期修少量编译错误 |
| 3 | npm 版本硬约束（`>=11.19 <12`），Arch 源 npm 是 12.x | 🟡 中 | fnm/nvm 按 `.node-version`=26.7.0 装环境，或单独装 npm@11.19 |
| 4 | gcc 16.2.1 远新于上游 CI 使用的 gcc 11–13，C++23 前端行为差异 | 🟡 中 | `BUILD_WERROR` 默认 OFF；如遇前端 bug 改用本机 clang 22（构建脚本区分 GNU/Clang） |
| 5 | Boost 精确 1.92.0 | 🟢 低 | CachyOS 当前正好 1.92.0；将来若不匹配会自动 FetchContent（需联网） |
| 6 | `SUNSHINE_REQUIRE_TRAY` 默认 ON，缺 appindicator 时构建 FATAL | 🟢 低 | 装 `libayatana-appindicator`+`libnotify`，或 `-DSUNSHINE_ENABLE_TRAY=OFF` |
| 7 | KWin Wayland 捕获受限（wlgrab 不可用） | 🟢 已知现实 | 用 kmsgrab（setcap）或 X11 会话（§七） |
| 8 | 直装路径没有 postinst，权限不会自动配置 | 🟢 低 | 按 §七手动 setcap + udev |
| 9 | 子模块工作树处于空置/全删除暂存的损坏状态（本机实测发生过：control-panel、Simple-Web-Server、TPCircularBuffer、ViGEmClient、moonlight-audio-haptics 五处 + 嵌套 enet/nanors 未检出；症状是 `add_subdirectory: does not contain a CMakeLists.txt`） | 🟡 中 | `git submodule update --init --recursive --force` 恢复（已执行）；编译前确认 `third-party/*/` 目录非空 |

---

## 十一、增强功能 Linux 迁移路线图（进阶）

目标从"能跑"升级为"尽可能恢复分支的增强体验"。原则：**优先复用分支已有的跨平台逻辑层**（大多
数增强功能的协议/会话/配置层本来就随主目标在 Linux 编译），缺的只是平台后端；个别功能（零拷贝
借帧）在 Linux 没有对等机制，明确放弃。

### P0 · 虚拟显示器 —— ✅ 已完成（2026-09-10，原生 C++ 后端）

> **完成情况**：最终实现比本节当初的调研更进一步——既没用 socket 守护进程（用户实测 NVIDIA+KWin
> 下黑屏，已弃用），也没走独立的 helper 二进制，而是 **in-process 原生后端 + 文件能力**：包
> postinstall `setcap cap_sys_admin,cap_dac_read_search,cap_dac_override,cap_sys_ptrace+p`，
> 进程内用 libcap RAII 把 permitted 提升为 effective，直接完成 EDID 生成/override、连接器强制、
> pidfd 借 DRM master、CRTC 指派与独占还原。实现明细见 §十二进度 7。以下保留当初的调研记录备查。
>
> **后续补强（2026-09-11）**：EDID 模式表从"主屏 + 5 档梯子"升级为配置列表的全组合（链式 CTA 块）、
> 显示名对齐 Windows、会话外创建的偏好模式改由 WebUI 配置列表推导、托盘 VDD 状态实时化并可 adopt
> 残留虚拟屏——见 §十二进度 18–21。

本机已装 `sunshine-virt-display-git r73`（frostplexx/sunshine_virt_display，位于 `/opt/sunshine-vd/`）：
root 守护进程（`sunshineVD.service`）监听 **Unix socket `/tmp/sunshineVD.sock`**，通过 EDID 覆盖 +
DRM 连接器开关动态创建/销毁虚拟输出；自带 **KWin 集成**（清理 `~/.config/kwinoutputconfig.json`
中的陈旧输出项）与 Hyprland 集成、logind 睡眠联动（jeepney/D-Bus）、多 GPU 卡自动选择；
要求 debugfs 挂载在 `/sys/kernel/debug/`。

**路线 A：零代码集成（先做，当天可用）** —— 官方设计就是通过 Sunshine 的 prep 命令触发，
不碰分支代码：

- Do Command（客户端连接）：
  `sh -c "echo --connect,--width,${SUNSHINE_CLIENT_WIDTH},--height,${SUNSHINE_CLIENT_HEIGHT},--refresh-rate,${SUNSHINE_CLIENT_FPS} | nc -U /tmp/sunshineVD.sock"`
- Undo Command（断开）：`sh -c "echo --disconnect | nc -U /tmp/sunshineVD.sock"`（`nc -U` 来自 `openbsd-netcat`）
- 前置：`systemctl status sunshineVD` 确认守护进程在跑；虚拟输出出现后 kmsgrab 抓它的 CRTC。

**路线 B：代码级移植（恢复分支 VDD 会话语义）** —— 分支的 VDD 生命周期逻辑
（创建/销毁时机、`vdd_keep_enabled`、`vdd_reuse`/"shared_vdd" 复用、按客户端物理尺寸、退出兜底销毁）
全在**跨平台**的 `src/display_device/session.cpp:143-293`，平台后端是 `vdd_utils`/`vdd_ioctl`。移植步骤：

1. **先修编译**：`src/display_device/vdd_ioctl.cpp` 无守卫 `#include <Windows.h>`/`<SetupAPI.h>`
   却在公共目标清单里 → Linux 必然编译失败（这是预期撞上的第一个具体错误）。把它（及
   `vdd_utils.cpp` 中的 Windows 片段：DevManView.exe 调用、原生确认对话框、`vdd_settings.xml`）
   移入 `_WIN32` 守卫或 `compile_definitions/windows.cmake` 的目标清单。
2. 实现 Linux 版 `vdd_utils` 后端：对话 `/tmp/sunshineVD.sock`（connect/disconnect，带分辨率/
   刷新率），映射 `create_vdd_monitor()/destroy_vdd_monitor()/wait_for_vdd_device()` 语义。
3. 复用 `config::video.vdd_*` 配置（现有键：`vdd_keep_enabled`、`vdd_reuse`、`vdd_headless_create`
   等）与 `system_tray::update_vdd_menu()`（托盘在 Linux 有实现）。

**与 Windows 版的功能差距（如实标注）**：Zako Direct 零拷贝借帧依赖 D3D11 共享纹理，Linux 无对等
机制（不可移植）；5 种屏幕组合模式中的"仅虚拟屏"可仿照其 Hyprland 思路在 KWin 侧隐藏物理屏实现；
HDR 亮度定制、按客户端 GUID 独立会话由 EDID/单虚拟屏方案近似替代。注：该工具的 EDID 生成器
**带 HDR 静态元数据块**（SDR+HDR+PQ，`src/edid/generator.py:185-188`），虚拟输出本身支持 HDR 能力声明。

### P0-C · 纯 C++ 原生集成（不依赖 Python 守护进程）

sunshine_vd 的机制**没有任何部分依赖 Python**，全部 1:1 映射到 C++，且所需库项目均已链接：

| 守护进程行为 | C++ 实现 |
|---|---|
| EDID 生成（含 HDR 静态元数据块、VIC 回退、像素时钟校验） | 纯字节构造 + 校验和，约 300 行；参考 `src/edid/generator.py`/`timing.py`/`vic.py` |
| EDID 覆盖、连接器状态强制（`/sys/kernel/debug/dri/.../edid_override`、`/sys/class/drm/cardN-PORT/status`） | `std::ofstream` 写 sysfs/debugfs |
| DRM 卡枚举、CRTC 抢占/释放/就绪等待 | libdrm —— **构建已有**（kmsgrab 依赖，`cmake/FindLIBDRM.cmake`） |
| 从合成器"借" DRM master（pidfd_open/pidfd_getfd + SET/DROP_MASTER） | `syscall()` 直调（内核 5.7+）或 libdrm；参考 `src/drm/drm_master.py` |
| KWin 陈旧输出配置清理（`~/.config/kwinoutputconfig.json`） | nlohmann_json —— **构建已有** |
| 睡眠前自动断开（logind D-Bus inhibitor） | sd-bus（libsystemd） |

**真正的约束是权限，不是语言**：上述 sysfs/debugfs 写入需要 root 级 DAC 权限；向 KWin 借 DRM
master fd 需要 `CAP_SYS_PTRACE`（绕过 yama）；CRTC 强制指派需要 `CAP_SYS_ADMIN`。Sunshine 本体
是普通用户进程（仅 `cap_sys_admin+p` 文件能力，为 kmsgrab），**不可能在进程内直接完成**。可行的
原生形态：

1. **推荐：一次性特权 helper**（如 `sunshine-vdd-helper connect --width W --height H --refresh F`
   / `disconnect`），编译为独立小二进制并
   `setcap cap_dac_override,cap_sys_admin,cap_sys_ptrace+p` 授权（与 sunshine 本体的 setcap 同一
   模式，无需常驻守护进程、无需 socket、无需 NOPASSWD sudo）。`vdd_utils` 的 Linux 后端用
   boost::process 调它 —— **这正是 fork 在 Windows 上的既有模式**（`vdd_utils.cpp:150-166` 用
   boost::process 调 DevManView.exe）。
2. 保守：C++ AF_UNIX 原生 socket 客户端对话已装的 Python 守护进程（`connect()` + write，约 30
   行，无子进程依赖），复用其已验证的睡眠/陈旧会话/NVIDIA 边界处理。
3. 不推荐：重写一个 root systemd 服务 —— 等价于方案 1 的常驻化，多一份生命周期管理成本。

**评估结论（定案）：目标形态选方案 1（原生 C++ helper），分两步落地。**

- **第一步**：先用方案 2 的 AF_UNIX 客户端（约 30–50 行）实现 `vdd_utils` 的 Linux 后端，对接已装
  守护进程，打通 `session.cpp` 挂接面与 `vdd_reuse`/`vdd_keep_enabled` 会话语义 —— 半天工作量即可
  让虚拟屏进入完整会话生命周期。
- **第二步**：实现 helper（EDID 生成 + sysfs/debugfs + libdrm + pidfd），把传输层从 socket 切到
  boost::process —— 后端接口不变，风险递进；socket 路径保留为 helper 缺席时的运行时回退。

选 helper 而非纯 socket 的理由：

1. **功能保真度是硬约束**：fork 的 VDD 增强语义是"按客户端定制 EDID"——
   `create_vdd_monitor(client, hdr_brightness, physical_size)` 要把 HDR 亮度和物理尺寸写进 EDID
   字节（客户端名 → 对角线英寸/PQ 峰值）。Python 守护进程的 socket 协议只有 W×H/FPS/device，
   永远承载不了这层；socket 方案只能拿到"有虚拟屏"，拿不到"分支的虚拟屏"。
2. **权限模型更干净**：一次性文件能力进程 vs 常驻 root 服务 + `/tmp` socket（socket 在世界可写
   目录，本地任意用户都能命令它断开你的虚拟屏）。
3. **与 fork 既有模式一致**：Windows 上 `vdd_utils` 就是 boost::process 调外部工具
   （DevManView.exe），Linux 版结构可以直接对齐，review/维护成本低。
4. **移植不是研发**：`/opt/sunshine-vd` 源码在手、机制经上游社区验证，Python 逻辑可 1:1 翻译
   （动手前先确认其 LICENSE 与 GPL-3.0 的兼容性，`/opt/sunshine-vd/LICENSE`）。

### P1 · 远程麦克风写主机 —— ✅ 已完成（2026-09-10）

`src/platform/linux/audio.cpp` 的 `write_mic_pcm()`/`init_mic_redirect_device()` 已实现：装载命名
null-sink `sink-sunshine-virtual-mic`（monitor 源即虚拟麦克风，主机应用直接选择录音），混音后的
48 kHz 单声道 PCM 经 pa_simple 写入，返回码契约对齐 Windows（正=字节/0=背压丢帧/负=设备丢失触发
重初始化）。模块参数已用 PipeWire 的 pulse 兼容层实测验证。

### P1 · 剪贴板主机侧集成 —— ✅ 已完成（2026-09-10，文本类）

`src/clipboard_host.cpp` 填补了 Windows 上由 GUI 代理（Tauri 面板 clipboard.rs）扮演的角色：
sd-bus 对话 KDE klipper（`org.kde.klipper` setClipboardContents/getClipboardContents），经
`clipboard_bridge` 与客户端双向同步，说与 GUI 代理相同的 10 字节文本帧协议（版本/kind/token/长度），
带 5 秒回声抑制与基线快照；`bridge_t` 新增 listener 列表使 GUI SSE sink 与主机 provider 共存，
`gui_alive()` 计入 listener 以便无 GUI 时仍广播剪贴板能力。**剩余**：图片（PNG/blob REF 类帧）写入
主机剪贴板、非 KDE 桌面的 provider（wlr-data-control / X11 XFixes）。

### P2 · HDR 动态元数据与 DV 亮度分析（复查后的修订版）

静态透传已确认跨平台可用（见 §八 ✅ 表），无需移植。动态元数据在 Linux 有**三个缺口**：

1. **逐帧亮度分析器** —— ✅ **HDR10+ 部分已完成（2026-09-10，`e49ae499`，tag
   `v0.7-linux-hdr10plus`，pkgrel 28）**：avcodec 路径的 HDR10+ 管线（side data 预挂、时间域
   滤波、`update_hdr_dynamic_metadata`）本来就是跨平台的，只缺统计生产端。软件设备 convert()
   现以 CPU 直方图遍历转换后的 P010/YUV444P10 亮度面产出全部统计字段（限幅范围重映射，
   PQ→nits 复用 `hdr_metadata::pq_to_nits`），门控 = PQ 会话 + `hdr_luminance_analysis`
   配置 + 客户端协商了 HDR10+。近似性：以亮度代替 maxRGB（Windows 是 scRGB 逐像素 RGB，
   消费端本就把 99 分位当 maxSCL 的近似）。**实测中纠正了两个 ST2084 常量错误**（m2 应为
   ×128、c3 应为 2392/4096×32=18.6875，对照参考表 100/1000/10000 nits 逐点验证）。
   **DV L1 RPU 在 avcodec 路径也已接入（2026-09-10，`24817d1a`，tag `v0.8-linux-dv-rpu`，pkgrel 29）**：
   跨平台注入器此前只挂在 NVENC/AMF 直连路径；avcodec 会话现按 NVENC 同款门控（分析可用 + 掌握
   元数据 + P8.1 需 PQ 基层；P8.4 需 HLG 在 KMS 路径明确拒绝）configure 注入器，encode_avcodec
   按提交帧序 stage L1，输出包按 pts 回程 splice RPU NAL（AVPacket 按需扩容）；分析器使能随之
   覆盖 DV 协商标识。分析器统计为亮度近似，RPU 的 L1 语义（min/avg/max PQ）与之天然匹配。
2. **HDR Vivid T.35 序列化器 —— ✅ 已完成（2026-09-11，`4595e349`）**：FFmpeg 只有 CUVA 解析器
   （bundled `libavutil.a` 符号核实：有 `av_dynamic_hdr_plus_to_t35`、无 Vivid 对应物），于是不再
   依赖 FFmpeg：`encode_avcodec()` 用共享 `serialize_vivid_t35()` 构建载荷，按提交帧序号暂存、
   输出包按 pts 经 `hdr_bitstream::append_t35_unit()` + `insert()` 插到首个 VCL NAL 之前，与 DV RPU
   共用一次 AVPacket 扩容；暂存队列有上限并带告警。**仅 Linux 编译**——Windows 的 avcodec 家族仍
   显式标记不能承载 Vivid，Vivid 走 NVENC/AMF 直连，行为不变。
3. **HLG 捕获源**：kmsgrab 不接受 HLG EOTF（`kmsgrab.cpp:840-841`），属上游内核/DRM blob 能力
   限制（`HDR_OUTPUT_METADATA` 仅定义 PQ/SDR），短期放弃；P8.4（HLG 基层）随 HLG 一起搁置。

### P2 · ABR 前台应用检测 —— ✅ 已完成（2026-09-10）

`src/platform/linux/foreground_app.cpp`：KWin 脚本经 D-Bus 把活动窗口（pid/resourceClass/caption）
事件推送到进程内 `org.sunshine.Abr` 服务，`detect_foreground_app()` 返回缓存；脚本静默（KWin 重启）
自动重装。Plasma 6 实测全链路通过；非 KDE 桌面维持空结果降级。

### P3 · NVENC SDK 直连（收益重估后再做）

`src/nvenc/common_impl/`（nvenc_base）是 OS 中立的，SDK 头文件（nvenc-headers 1100/1200/1300/1301）
也已在树；Windows 专属的只是 `win/` 下的 D3D11 工厂。理论上写一个 `linux/` 工厂（CUDA context +
EGL/CUDA 导入 + NvEncRegisterResource）即可恢复 SDK 直连。但 FFmpeg 的 nvenc 封装的就是同一套
SDK API，直连的增益主要是 fork 的细粒度码控/lookahead（探测缓存是跨平台的，不在此列）——若 P2
完成，此项优先级可降。工作量：高。

### P3 · 其余项

| 功能 | Linux 对等路径 | 工作量 |
|---|---|---|
| Tauri 控制面板（深入评估后修订） | **结构上已为跨平台做准备**：Windows 依赖全部隔离在 `[target.'cfg(windows)'.dependencies]`（windows/clipboard-win/wmi/winrt-notification），核心依赖（tauri 2.11、reqwest、axum、xcap、clipboard-rs）均跨平台；但约 20 个系统模块仅有 Windows 实现（vdd/vigem/vmouse/rtss/usbip/hwinfo(WMI)/elevation/注册表自启/dualsense elevated 等），全库仅 1 处 `cfg(not(windows))` 回退 → Linux 构建需逐模块补后端或直接裁剪模块声明 | 中-高，收益低（WebUI 已覆盖大部分；值得选择性移植的只有 QR 配对、实时监控、托盘增强；Linux 需 webkit2gtk-4.1） |
| DS5 sidecar（主机直连手柄播放触觉 PCM） | `/dev/hidraw` 直写 DualSense 输出报告；基础 rumble 已可用 | 高 |
| 文件共享"右键共享"入口 | KDE ServiceMenu（.desktop 服务菜单）调 file_mapping 的 RPC | 低（锦上添花） |
| vmouse 虚拟鼠标 | inputtino 已提供 uinput 绝对鼠标/触摸/笔 | 基本冗余 |
| Zako Direct 借帧 / WGC / rtx_hdr bridge | 无 Linux 对等机制 | **放弃** |

---

## 十二、建议的实施顺序

**基础迁移（§一–§十）**：

1. **恢复模板**（§三，4 个必需文件即可）→ 2. **装缺失包**（cmake、ninja、boost、numactl、
   libayatana-appindicator、node/npm 环境）→ 3. **配置构建**（`-DSUNSHINE_ENABLE_CUDA=OFF` +
   `-DBUILD_TESTS=ON`）→ 4. **编译 + 跑测试**，修复预期中的编译错误——**第一个就是 §十一 P0-B 的
   `vdd_ioctl.cpp` Windows 头文件问题** → 5. `ninja install` + setcap/udev → 6. KDE Wayland 下用
   kmsgrab 实测串流 → 7. 稳定后补 `Arch/PKGBUILD` 恢复，转 makepkg 管理，并考虑加 Linux CI。

**进度（2026-09-09，`linux-migration` 分支）**：基础迁移的第 1–4 步已完成 ——

1. 模板已恢复并提交（`1b15942`）。注意上游 master 已把模板改名为
   `dev.lizardbyte.app.Sunshine.*` 布局，须取旧布局的最后一版（上游 commit `b2d44f5b`）。
2. CMake 层修复（`6bb63af`）：托盘改接 `tray::tray` 目标（固定的 tray 新版在 Linux 是
   **Qt6 + libnotify 实现**，需要 `qt6-base`/`qt6-svg`，KDE 系统已自带）；删除 `input.cpp` 死引用。
3. 全量编译修复（`b719edf`）：vdd_ioctl/vdd_utils 的 Windows 实现加 `_WIN32` 守卫 + Linux 桩、
   globals 的 ZAKO_NAME 等移出守卫、config.cpp 补 QVBR/HQVBR 常量（4/5/6）、video.cpp 的 avcodec
   NVENC 选项表改用 SDK 数值（该分支此前从未在任何平台编译过）、Boost 增加 regex 组件、测试 Glob
   排除 Windows 宿主目录等 —— GCC 16 / C++23 下 **`build/sunshine` 与 WebUI 全部构建成功**。
4. 测试：13 个 ctest 套件 12 个直接通过；聚合套件 **0 个断言失败**，仅 Audio/MouseHID/Encoder
   三个套件因需要真实音频/输入/编码器环境在 SetUp 失败（与上游行为一致，需在图形会话内跑）。
5. 打包与实测（`b385f83`/`833d523`/`2ff343f`）：本地调试包 `packaging/arch-local/`（装到
   `/opt/sunshine`，post-install 自动 setcap + udev）；修复托盘菜单越界段错误后实测——端口
   47984/47989/47990 监听、WebUI 200、托盘创建、**h264/hevc_nvenc 探测通过**（av1 被 Ampere
   正确拒绝）、KMS 捕获工作。
6. **P0 虚拟显示器第一步已落地（`1a3110a`）**：`vdd_utils` 的 Linux 后端通过 Unix socket 对接
   已装的 sunshineVD 守护进程（connect/disconnect、按缓存会话模式重连、状态文件+/sys 判活），
   `vdd_capability` 改为全平台统一状态映射 —— 守护进程在跑时客户端即可请求虚拟显示器。
   已知守护进程侧局限：NVIDIA+KWin 下 disconnect 的物理屏 CRTC 恢复会失败（上游仅对 Hyprland
   做了特殊处理），虚拟输出可能残留，重试断开或重启系统可清理。
   待办（P0 第二步）：原生 C++ helper（EDID 生成 + sysfs/debugfs + libdrm + pidfd）替换 socket
   传输，以支持按客户端 HDR 亮度/物理尺寸定制 EDID。——（此待办已在下一步完成，但走的是
   in-process 后端而非 helper 二进制，socket 路线整体弃用。）
7. **P0 第二步落地：原生虚拟显示器后端（pkgrel 17–19，`c0988e87`/`e277996b`/`52329f9d`/
   `3257b18e`/`9f9c271a` 等）**。用户实测 socket 路线在 NVIDIA+KWin 下黑屏后整体弃用，改为
   in-process 原生后端，对齐 fork 的 ZakoVDD 语义：
   - **个性化 EDID**：`src/platform/linux/vdd_edid.*`（由 sunshine_vd 的 Python 生成器 1:1 移植），
     显示名 "Foundation Display"、按客户端物理尺寸、可选 HDR 静态元数据块（CTA-861 亮度编码）、
     主屏模式表 + 兜底梯子；`tests/unit/test_vdd_edid.cpp` 与 Python 参考逐字节对照（7 用例全过）。
   - **连接器操控**（`src/display_device/vdd_utils.cpp` Linux 段）：debugfs `edid_override` 写入、
     sysfs `status` 强制 on/off、`offlined_physical_status_paths` 跟踪与 `restore_offlined_physicals()`
     （独占模式退出还原，用户已实测确认）、残留 VHD 连接器清扫（EDID 签名 `00FF...005624`）。
   - **CRTC 强制指派**：`SYS_pidfd_open`/`SYS_pidfd_getfd` 从合成器借 DRM master + dumb buffer +
     `drmModeSetCrtc`，保证虚拟输出实际点亮并被捕获。
   - **权限模型**：包 postinstall `setcap cap_sys_admin,cap_dac_read_search,cap_dac_override,cap_sys_ptrace+p`，
     进程内 libcap RAII（permitted→effective）按需提升——比原计划的 helper 二进制更简单直接。
   - **6 种 prep 模式**：`vdd_prep_e` 全量对齐（独占=物理屏下电+退出还原；扩展类=kscreen priority
     排布）；`parsed_config` 对 `23172`/`ZAKO_NAME` 设备 id 特判 `explicit_vdd`。
   - **桌面环境感知**：主屏语义经 `XDG_CURRENT_DESKTOP` 分发（KDE → kscreen-doctor），不绑死 KDE，
     其他 DE 记录日志并降级。
   - **会话周边**：capability_version 真实上报（客户端可见虚拟屏选项）、托盘 Foundation Display
     菜单解禁、SIGINT/SIGTERM 唤醒托盘事件循环、kmsgrab 按连接器名定向捕获虚拟屏（VD 优先级 >
     名字匹配 > 数字索引回退）、AMF 常量表补 QVBR/HQVBR/HQCBR。
8. **向导与状态面**（`52329f9d`/`3257b18e`/`9f9c271a`）：WebUI 初始化向导 Linux 平台化文案（去掉
   ZakoVDD/Win10 22H2 硬性提示）、GPU 选择接通捕获、`/api/vdd/status` 真实状态、`adapter_names()`
   返回真实 DRM 驱动名（nvidia/amdgpu/i915）。
9. **历史治理**：误提交的 makepkg 缓存/产物用 filter-branch 清除并 gc（pack 31.71 MiB）；重写导致
   上游基线哈希级联漂移，从 fork master 嫁接原版 rebase 修复。
10. **打包（pkgrel 21，`48b7c4bf`）**：安装前缀改为全局 `/usr`（`SUNSHINE_ASSETS_DIR` 编译期路径
    随之固化，全量重编）；图标对齐 fork（fork 的 PNG 套装装入 hicolor 16/256，apps+status）；
    桌面文件 `--u`→`--user` 笔误修正；appdata 移除上游截图 URL。产物
    `sunshine-foundation-2026.0909-21-x86_64.pkg.tar.zst`（独占/还原/实体屏/虚拟屏串流用户已实测）。
11. **物理显示器 display_device 后端（pkgrel 22，`09a49cf1`，tag `v0.3-linux-display-device`）**：
    `src/platform/linux/display_device.cpp` 从全 stub 重写为真实实现——kscreen-doctor 作为合成器
    中介读写模式/HDR/主屏/拓扑（含 `output.N.mode.WxH@refresh` 语法坑：小数刷新率须回退模式 id）、
    JSON 持久化（`original_display_settings.json`）与 apply/revert 流程对齐 Windows settings.cpp；
    VDD 拓扑保持由会话 VDD 阶段控制；kscreen 不可用的环境自动退化为原 no-op 行为。解析器用真实
    `kscreen-doctor -o` 输出（含 ANSI 码）单独验证。
12. **主机侧剪贴板同步（pkgrel 23，`a926876d`，tag `v0.4-linux-clipboard-host`）**：
    `src/clipboard_host.cpp`（libsystemd sd-bus ↔ klipper，可选编译）+ `bridge_t` listener 机制；
    文本双向同步，回声抑制 5s TTL，会话开始时基线快照；复用 GUI 代理的 10 字节文本帧协议。
13. **远程麦克风写主机（pkgrel 24，`db4e2f40`，tag `v0.5-linux-mic-redirect`）**：
    命名 null-sink `sink-sunshine-virtual-mic`（monitor 源即虚拟麦克风）+ pa_simple 写入，
    返回码契约对齐 Windows VB-Cable 路径；模块参数经 PipeWire pulse 兼容层实测。
14. **重启问题两轮修复（pkgrel 25–26，`26f826e1`/`c203f020`，tag `v0.5.1-linux-restart-fixes`/
    `v0.5.2-linux-tray-restart`）**：托盘重启三重缺陷——升级后 `/proc/self/exe` 失效（回退
    argv[0]）、AT_SECURE 下 sd-bus `secure_getenv` 失效（显式构造 bus 地址）、popen 子进程继承
    监听 socket（有界 fork/exec + acceptor CLOEXEC）；第二轮找到真正根因：非主线程的重启入口
    从不唤醒停在托盘事件循环的主线程，init_tray 现挂 shutdown 监视线程。
15. **ABR 前台应用检测（pkgrel 27，`d504fa11`，tag `v0.6-linux-abr-foreground`）**：
    KWin 脚本（D-Bus 加载、静默自动重装）把活动窗口变化推送到进程内 `org.sunshine.Abr` 服务，
    ABR 轮询读缓存（pid/resourceClass/caption）；Plasma 6 上 loadScript/run/windowActivated
    信号与回传链路实测通过；非 KDE 桌面保持原有空结果降级。
16. **HDR10+ 逐帧亮度分析器（pkgrel 28，`5376dff7`，tag `v0.7-linux-hdr10plus`）**：
    avcodec 路径的 HDR10+ 管线（side data 预挂、时间域滤波、`update_hdr_dynamic_metadata`）本就是
    跨平台的，缺的只是统计生产端。软件设备 convert() 现以 CPU 直方图遍历转换后的 P010/YUV444P10
    亮度面产出全部统计字段（限幅范围重映射，PQ→nits 复用 `hdr_metadata::pq_to_nits`），门控 =
    PQ 会话 + `hdr_luminance_analysis` 配置 + 客户端协商 HDR10+。**实测中纠正两个 ST2084 常量错误**
    （m2 应为 ×128、c3 应为 2392/4096×32=18.6875，按参考表 100/1000/10000 nits 逐点验证），常量现
    收敛在 `video_hdr_metadata.h` 的 `detail::st2084_*`。
17. **DV P8.1 RPU 接入 avcodec 路径（pkgrel 29，`24817d1a`，tag `v0.8-linux-dv-rpu`）**：
    跨平台注入器此前只挂 NVENC/AMF 直连路径；avcodec 会话现按 NVENC 同款门控 configure（P8.1 需
    PQ 基层；P8.4 的 HLG 在 KMS 路径明确拒绝），按提交帧序 stage L1、输出包按 pts 回程 splice RPU
    NAL（AVPacket 按需扩容）；分析器使能随之覆盖 DV 协商标识。分析器为亮度近似，与 L1 的
    min/avg/max PQ 语义天然匹配。
18. **托盘 VDD 状态与消息框（pkgrel 30–31，`387a97fd`/`a2c2deef`，tag `v0.8.1-linux-tray-vdd-state`/
    `v0.8.2-linux-tray-msgbox`）**：修两处用户可见缺陷——Qt 托盘需 `checkbox=1` 才渲染勾选态
    （Windows 仅凭 `checked` 画 MFS_CHECKED），且菜单状态只在 init/托盘动作时计算；现每 2s 轮询
    `is_vdd_active()` 并变化即刷新，首次查询时 **adopt** 进程外残留的虚拟屏（扫描带 VHD 签名 EDID
    的连接器、还原跟踪的连接器路径、从 DTD 解析回偏好模式——EDID override 与强制连接器状态是
    DRM 持久的，进程内簿记却随进程消失）。危险操作（保持启用 / 无显示器自动创建 / 重置显示配置 /
    退出）改走 Qt `QMessageBox` 确认，语义对齐 Windows `MessageBoxW`；构建面用 pkg-config 解析
    Qt6Widgets（二次 `find_package(Qt6)` 会破坏 Qt6 的 config 文件）。
19. **手动虚拟屏模式参数化（pkgrel 32–33，`4e186f4b`/`2ce99874`，tag `v0.8.3-linux-manual-vdd`/
    `v0.8.4-linux-vdd-mode-list`）**：会话外创建（托盘创建、无显示器自动创建）不再固定
    1920x1080@60。先引入 `vdd_manual_resolution`/`vdd_manual_fps` 两个键，随即删除——直接取 WebUI 的
    `config::nvhttp.resolutions`/`fps`（Windows SETMODES 的同一数据源）中面积最大分辨率 × 其最高
    可行刷新率，无效配置回退 1920x1080@60；客户端会话配置过模式后仍优先（`cached_from_session`，
    等价于驱动保留上次模式）。无新增配置键。
20. **虚拟屏命名与解析边界对齐 Windows（pkgrel 34–35，`89cd047c`/`e3d11b37`，tag
    `v0.8.5-linux-vdd-name`/`v0.8.6-linux-parity-audit`）**：EDID 名描述符原为 "Foundation VDD"，
    被 13 字节描述符上限截成 "Foundation VD"，KDE 再前缀 PnP 厂商字母，渲染为 "UQD Foundation VD"；
    改用共享常量 `ZAKO_NAME`（"Zako HDR"），UQD 前缀是厂商 ID 字节的固有解码（该字节同时充当残留
    VHD 清扫/adopt 签名）。随后消除一处重新拼写的字面量，并撤掉自创的配置解析夹取
    （640–8192 / 480–8192 / fps 24–480），对齐 Windows `parse_vdd_resolution`/`parse_vdd_refresh_hz`
    的宽松接受（任意正值 + x/X 分隔符），可行性过滤留给 EDID 生成器（对应 Windows 侧留给驱动）。
    规则沉淀进 `AGENTS.md` 的 Windows parity 一节（共享常量、语义含边界、用户可见命名三条）。
21. **完整模式表：配置全组合 × 链式 CTA 块（pkgrel 36–38，`49800f6c`/`4ad74c90`/`fbb1b911`，tag
    `v0.8.7-linux-vdd-refresh-ladder`/`v0.9.0-linux-full-mode-table`/`v0.9.1-linux-full-cross-product`）**：
    三步收敛——
    ① 阶梯原为"每个分辨率只配最高配置刷新率"，导致 1080p 客户端只见 144 Hz，且首选模式选到
    3840x2160@144（像素时钟溢出 CEA-861 上限，被静默回绕成 71.38 Hz 的垃圾时序）；改为**可行组合
    的叉积**，首选分辨率保留全部配置刷新率，首选选择跳过不可行组合。
    ② 单个 CTA 扩展块上限 6 个模式，改为**链式扩展块**：块 1 = 数据块 + 首选时序副本 + 4 个附加，
    后续 DTD-only 块各 6 个，每块独立校验和与 DTD 起始指针；Range Limits 描述符同步覆盖全部通告
    刷新率（此前写死 preferred±20，与 144 Hz 模式自相矛盾；此变更使旧的逐字节参考向量失效，
    见下方测试基线）。
    ③ 非首选分辨率同样携带全部配置刷新率，达成与 Windows SETMODES 等价的全组合。
    像素时钟模型收敛为 `vdd_edid::dtd_pixel_clock_hz()` 单一来源（DTD 生成、首选模式编码、导出的
    `mode_fits_pixel_clock_limit()` 过滤共用）。用户真实配置实测（fps 60/90/120/144，720p…4K）：
    640 字节 / 4 扩展块 / 24 DTD / edid-decode 零告警——4K@60 首选（144/120/90 超像素时钟上限被
    过滤）+ 3440x1440@90/60 + 其余分辨率 @144/120/90/60 全档。
22. **Windows 构建面保持绿色（`3756caf6`）**：`vdd_edid` 是 Linux-only 目标文件，其逐字节对照测试
    在 `tests/CMakeLists.txt` 的 `if (WIN32)` 分支按文件名排除；本轮未向 `SUNSHINE_TARGET_FILES`
    新增任何 Linux 源，共享代码改动均保持在 Windows 行为恒等（`ZAKO_NAME` 引用、托盘 `checkbox`
    字段是 tray 结构体的既有成员）。
23. **已移植部分的 Windows 对齐审计（2026-09-11）**：对 VDD / display_device / 剪贴板 / 麦克风 /
    ABR / 托盘 / HDR 编码七块做逐行对照审计，修复 15 项真实偏差，完整清单与证据见
    `LINUX_PORT_GAPS.md` §五。要点：
    - **VDD**：模式表 8 个 helper 从 `_WIN32` 段提到共享段（逐字搬移），Linux 阶梯改用
      `prepare_vdd_settings()`（原为返回 `{}` 的桩），解析语义对齐 Windows（trim、拒绝尾随字符、
      小数刷新率四舍五入 59.94→60）；per-client 尺寸类表收敛为共享
      `client_physical_size_for_class()`；`create_vdd_monitor` 不再丢弃客户端物理尺寸与 HDR 亮度
      ——EDID 的尺寸描述符与 HDR 静态元数据块现按 CREATEMONITOR 载荷同源写入。
    - **display_device**：空 device_id（Display: Auto）解析为主屏（kscreen priority 1），模式/HDR
      不再作用于所有输出、`ensure_only_display` 不再空转；刷新率容差 0.051 Hz → Windows 的 1 Hz
      模糊比较并取最近候选（59.94 面板 + 60 fps 不再配置失败）；HDR 使能对齐 Windows 的"仅
      disabled==disabled 跳过"（读回 enabled 可能是陈旧值）。
    - **HDR 分析**：修正亮度分析器的 10-bit 解包——平面格式（YUV444P10LE/YUV420P10LE）是低位对齐，
      原先套用 P010 的 `>>6` 会读成全黑并仍标记 valid（输出近零亮度元数据），现按格式掩码并接受
      YUV420P10LE；删除 avcodec 路径上"Dolby Vision 协商但无 RPU"的过时告警（P8.1 现已注入）。
    - **剪贴板/麦克风/ABR/托盘**：Linux 不再通告 `clipboard_image`（provider 仅文本）；单口味文本帧
      token 固定 0；`microphone_redirect_backend=disabled` 生效、写错误码区分设备丢失与一般错误；
      KWin 缺 pid 时不再冻结 `foreground_exe`；托盘确认框默认按钮与 warning 图标对齐 Windows。
    - 审计同时确认若干块**无差异**（剪贴板帧布局/阈值/TTL、麦克风采样与返回码形状、前台字段形状、
      托盘菜单结构、display_device 持久化 schema 与 apply/revert 编排、ST2084 常量全树唯一来源），
      并列出待决策项（如 VAAPI/CUDA 会话缺亮度分析源、HLG 无分析源、麦克风背压与默认录音设备切换、
      复制拓扑、枚举 active 语义等）——见 `LINUX_PORT_GAPS.md` §5.2。
24. **第二轮：niri 目标立项 + Vivid 收官 + 规范标识符收敛（2026-09-11）**：
    - **HDR Vivid 上 avcodec 路径（`4595e349`）**：FFmpeg 无 CUVA 序列化器（bundled `libavutil.a`
      符号核实），改为在 `encode_avcodec()` 内构建 T.35 并按 pts 拼接（与 DV RPU 共用一次 AVPacket
      扩容），新增位流回归用例；**仅 Linux 编译**，Windows 的 avcodec 家族仍标记为不能承载 Vivid。
      至此 Linux 的动态 HDR 三件套（HDR10+ / DV P8.1 / Vivid）齐备（§十一 P2 第 2 条）。
    - **麦克风默认录音设备（`e7392d98`）**：Windows 会切换默认录音设备并在结束时还原，Linux 原先只
      建 null-sink，主机应用需手动选 monitor 源。现复用文件内 pa_context helper：读当前默认源 →
      指向 null-sink monitor → 释放时还原；对 PulseAudio/PipeWire 通用，不是 KDE 专属。
    - **规范标识符收敛（`e4563139`）**：客户端"虚拟屏"占位 id 由 Linux 分支裸写的 `"23172"` 改为共享
      `ZAKO_DEVICE_ID`（globals，Windows 分支行为不变）；剪贴板线协议常量（版本/kind/帧头/内联阈值/
      TTL）移入 `clipboard_bridge.h`，`clipboard_host.cpp` 全面改用（Rust agent 仍是跨语言参考）。
    - **目标环境（约束）**：`AGENTS.md` 写明 **KDE + niri** 双目标、通用方案优先、后端必须探测并
      优雅降级；niri 的具体差距（前台检测 producer、通用输出后端 wlr-output-management、niri 输出
      控制）立项为 `LINUX_PORT_GAPS.md` §2.11。本机未装 niri，相关命令需在 niri 环境实测。
25. **第三轮：niri 起步（2026-09-11）**：
    - **ABR 前台检测的 niri producer**：`foreground_app` 新增 niri 路径——轮询
      `niri msg --json focused-window`（2 s，`run_logged` 有界执行），JSON 逐字段、带类型检查地
      映射到 `info_t`（title/app_id/pid）；`null`（无焦点窗口）不更新缓存，与 KWin 脚本"只在
      切换时上报"及 Windows 空前台窗口的语义一致。**后端选择在启动时决定**：先探测
      `org.kde.KWin` 是否在会话总线上（NameHasOwner），KDE 保持原 KWin 路径；无 KWin 且 niri 查询
      可用才启用 niri。解析器带 4 个单元测试（正常 / `null` / 字段缺失或类型错误 / 畸形 JSON）。
    - **VDD 输出启用的合成器无关化**：新增 `enable_output_via_compositor()`——KDE 仍是
      kscreen-doctor（行为不变），niri 走 `niri msg output <name> on`，其他 Wayland 走
      `wlr-randr --output <name> --on`（wlr-output-management，覆盖 sway/Hyprland/river…），
      X11 走 `xrandr`；都没有则只记一次 info 并依赖合成器自动启用。`hint_primary_output` 对 niri
      明确记录"niri 无主屏概念"。DRM 层 CRTC 指派仍是点亮输出的主路径，这些命令失败不影响现状。
    - **仍待做**：display_device 的 niri / wlr-output-management 输出后端（模式/HDR/拓扑），
      以及 wlroots/X11 的前台 producer——见 `LINUX_PORT_GAPS.md` §2.11 待做项与 §2.6。
26. **第四轮：display_device 失败处理对齐 Windows（2026-09-11）**：审计项 D5/D8/D9/D10/D14 落地，
    D13 复核为已由构造覆盖，D7 判定不移植：
    - **模式（D5）**：`set_display_modes()` 改为 Windows 的三段式——模糊（1 Hz）+ 最近模式应用 →
      复核全部匹配 → 仍不匹配则以**精确刷新率**重试一次（等价于去掉 `SDC_ALLOW_CHANGES`，让用户
      自定义模式也能选中）→ 再失败则把进入时的 `original_modes` 整体回滚并返回 false（不再留下
      半套模式）。
    - **HDR（D9/D10）**：`set_hdr_states()` 先快照 `original_states`，任一设备失败即回滚；请求的
      设备在读回中没有任何 HDR 信息时立即以明确日志返回 false（Windows `device_hdr_states.cpp`
      语义），不再对注定失败的命令重试 3 次。
    - **拓扑（D14）**：`set_topology()` 把 enable/disable 循环拆成 `apply()`，应用后经
      `wait_for_topology(3 s)` 收敛再复核，确实不一致则回滚到进入时的拓扑并返回失败；合成器完全
      无响应（读回为空）时保持历史容忍，避免误判。
    - **稳定性等待（D8）**：新增 `wait_for_display_stability()`（10 × 500 ms，上限 5 s）——本调用
      改过拓扑/模式时，先等目标设备的模式与 HDR 状态都可读再切 HDR；超时只告警继续（与 Windows
      一致），同时防止"读回滞后"把上面的快速失败变成硬失败。
    - **复核结论**：D13（还原顺序 + 还原后 HDR 复核）在 Linux 已由"先恢复初始拓扑、再按当前启用
      设备过滤后还原 HDR/模式"的构造覆盖，无需改顺序，避免动到用户已实测的还原路径；D7
      （blank HDR toggle）是 Windows 显示栈（IDD/VDD）的"颜色发白"清理手段，Linux 的 VDD 是真实
      DRM 连接器、无该症状，**有意不移植**（如实测出现同症状再补）。

27. **第五轮：麦克风背压契约落地（2026-09-11）**：Windows 后端从不阻塞共享麦克风线程（查端点
    剩余缓冲，满则返回 0 丢帧），Linux 原先直接 `pa_simple_write` 阻塞写——卡住的 sink 会拖住混音
    线程（并推迟 mix 定时器），且 `stream.cpp` 的 `wasapi_backpressure_drops` 恒为 0。现改为
    **有界队列 + writer 线程**（`src/platform/linux/mic_queue.h`，容量 5 帧 = 100 ms，与 Windows
    端点缓冲同量级）：`write_mic_pcm()` 只入队，永不阻塞；队列满返回 0（文档约定的背压丢帧），
    阻塞写与错误码映射（-2 设备丢失 / -1 一般错误）移到 writer 线程并在下一次调用上报，调用方的
    重初始化契约不变。队列逻辑带 7 个单元测试（FIFO/满队列/stop 唤醒与丢弃/reset 重武装等）。
    说明：析构与 release 先 stop+join writer 再销毁流；若音频服务端彻底僵死，join 仍可能被写阻塞
    ——与改造前"写卡在会话线程"的暴露面相同，未加重。

28. **第六轮：HDR 亮度分析覆盖面补齐（2026-09-11）**：此前亮度分析器只挂在 avcodec 软件设备上
    （`convert()` 内），而 Linux 的 VAAPI/CUDA 采集内存类型会走各自的硬件编码设备（`data != nullptr`），
    这些会话既无统计、`hdr_luminance_analysis_available` 也保持 false → AMD/Intel（VAAPI）与开启 CUDA
    的构建拿不到 HDR10+/DV/Vivid。现新增**采样下载**生产者：PQ + 请求 HDR10+/DV P8.1 时，每 4 帧用
    `av_hwframe_transfer_data()` 下载一帧到缓存软件帧并复用同一 CPU 分析器（采样间隔对齐 Windows 的
    1/4）；下载格式非 10-bit 或传输失败即关闭本会话分析并告警，退化为"无动态元数据"。能力判定与采样
    整体在 `#if !defined(_WIN32)` 内，Windows 由采集设备自产统计、标志恒 false，行为不变；Linux 软件
    设备路径同样不受影响。**未在 VAAPI/CUDA 硬件上实测**（本机 NVIDIA + `SUNSHINE_ENABLE_CUDA=OFF`），
    代价是全分辨率下载约 90 MB/s@1080p60（详见 `LINUX_PORT_GAPS.md` §5.8）。

29. **第七轮：枚举语义与剪贴板写入（2026-09-11）**：
    - **设备 active 语义对齐 Windows（D12）**：`enum_available_devices()` 改为读 DRM sysfs 的
      `enabled` 属性（CRTC 已绑定，等价 Windows `DISPLAYCONFIG_PATH_ACTIVE`），因此**连上但被桌面
      禁用**的显示器报 inactive，不再被 VDD 保活逻辑重新点亮；属性不可读时回退旧的"已连接即 active"，
      虚拟屏保持"live 即 active"（其通路由本后端管理）。带 2 个基于真实 sysfs 的规则测试。
    - **剪贴板写入不再阻塞控制线程（C3）**：客户端→主机的写入改为有界队列（8 条，溢出丢最旧），
      由 provider 的 poll 线程用同一个 bus 执行；等待改为条件变量（写入即唤醒，`stop()` 也唤醒）。
      enet 控制线程因此不再做同步 D-Bus 调用，klipper 僵死不会拖住控制包处理；回声抑制仍在入队时
      记录，写成功后同步 `last_seen` 避免回发自己写的内容。

30. **第八轮：剪贴板大文本 blob 回退（2026-09-11）**：主机侧 provider 此前对超过 60 KB 的文本
    直接丢弃。现补齐两条方向——主机→客户端改为存入 blob store 并发送 `kKindRef` 描述符帧（对端从
    本机 blob HTTP 端点取字节）；客户端→主机的 `kKindRef` 按 GUI agent 语义解析描述符、校验 id、
    只接受 `text/*`、从本地 blob store 取回后走既有的回声抑制与写入队列。同时把线协议编解码抽成
    `src/clipboard_wire.h`（逐条对照 Rust agent 的 `encode_frame`/`decode_frame`/`RefMeta`，MIME 与
    id 上限放 `clipboard_bridge.h` 共享），并补 6 个跨平台单元测试。图片（KIND_PNG）与文件投递
    （KIND_FILE_OFFER）在 Linux 主机侧仍不支持（§1.4/§2.2）。

31. **第九轮：剪贴板回声环形与事件驱动（2026-09-11）**：
    - **回声抑制改为 agent 同款环形（C6）**：新增 `src/clipboard_echo.h`（16 项 `(kind, payload-hash)`
      + TTL，满则丢最旧、检查前剪枝），provider 换用它；连续两次客户端写入后，主机复制较早那个值
      不再被回广播。5 个跨平台单元测试。
    - **klipper 变更事件驱动（C7）**：订阅 `clipboardHistoryUpdated` 信号（`sd_bus_add_match`）并每轮
      `sd_bus_process` 排空总线，等待缩短到 200 ms（排队写入仍即时唤醒）；1 s 周期读保留为兜底，
      信号不可用时行为与之前完全一致。新 sd-bus 用法已在真实会话总线上用独立程序验证（规则被接受、
      fd/process 行为符合预期）。

32. **第十轮：托盘高级设置与退出文案（2026-09-11）**：Linux 托盘此前没有 Advanced Settings 子菜单，
    且导入/导出/重置配置三个回调是"未实现"桩、另有两处实现不可达。现在子菜单两平台布局一致（导入/
    导出/重置为默认/清理缓存/重置显示器），三个配置操作按 Windows 语义实现（安全校验、`.backup`
    备份、`.tmp`+`rename` 原子替换、导入后询问重启），清理缓存补齐确认框，`update_menu_texts()`
    的平台分叉合并为一份索引表。新增 Linux 专用退出文案键（不再提不存在的 GUI 应用）与两个对话框
    文案键（三语）。Windows 的菜单语句、回调与文案逐字未变。

33. **第十一轮：占位 HDR10+ 元数据与前台缓存失效（2026-09-11）**：
    - **不再发送占位 HDR10+ SEI（F5）**：avcodec 路径原先在会话建立时就预挂 HDR10+ side data（值为
      "满亮度"占位），只有拿到有效统计才覆盖——于是**没有分析器的会话整场**、以及**首帧统计到达前的
      几帧**都会把占位值当真实测量发出去。现改为懒挂载：第一帧有效统计到来时才创建 side data 并
      初始化静态字段（窗口/椭圆/色调映射），会话新增 `hdr10plus_side_data_wanted`（承载能力 ∧ 分析器
      可用）。与 Windows 原生路径"只有 valid 统计才构造元数据"对齐；Windows 侧默认配置仅少了
      "首帧前的伪造 SEI"，分析关闭时不再发占位块（与原生路径一致），判定为修正。
    - **前台缓存失效（F2）**：新增 `clear_cache()`——KWin 脚本重载失败、niri 查询失败（生产者已死）时
      清空缓存，ABR 退回基于启动器的分类，不再按已关闭的游戏调码率；长时间不变的前台窗口不会被
      误清（有意保留）。

34. **第十二轮：分析节奏与上限对齐（2026-09-11）**：Linux 的 CPU 亮度分析此前**每帧全分辨率**运行，
    而 Windows 是 1/4 帧采样——共享时间滤波（EMA、场景检测、Vivid 启动门）以 `sample_sequence`
    识别新样本，因此两侧对同一内容的元数据动态差异达 4 倍，Linux 的 CPU 开销也偏高。现把采样间隔
    抽成共享常量 `hdr_metadata::hdr_analysis_interval`（Windows `display_vram.cpp` 与 Linux 软件路径、
    硬件下载路径统一引用），Linux 改为与 Windows 同节奏采样，开销降到 1/4，元数据动态一致；
    `analysis_max_nits` 的裸 10000 也换成共享 `st2084_peak_nits` 并注明 HLG 应有的取值规则。
    残留：CPU 仍读整帧（Windows 在 GPU 端缩到 ≤1080p），4K 下采样量约为其 4 倍；Windows 分析异步
    陈旧、Linux 同步当帧的差异保留。

35. **第十三轮：friendly name 与合成器降级语义（2026-09-11）**：
    - **显示器 friendly name（D17）**：新增 `src/platform/linux/edid.h`（解析 EDID 基块的 Display
      Product Name 描述符，纯字节变换、可单测）；枚举的 `friendly_name`、`get_display_friendly_name()`
      改用 EDID 名（无该描述符时退回连接器名），`find_device_by_friendlyname()` 改为遍历所有设备匹配
      （Windows 同语义，虚拟屏保留 ZAKO_NAME 快路径）。WebUI 设备列表因此显示型号而非 `DP-1`，配置里
      的显示器字段也能用型号解析。本机内建面板的 EDID 恰好没有名字描述符（真实常见），已验证会正确
      退回连接器名。5 个单元测试（含用自家 EDID 生成器做往返）。
    - **合成器不可用（D18）**：复核后**有意保留 success 结果**（Linux 上这是会话的持久属性，返回失败会
      让所有非 KDE 会话进入 deferred-retry 并给客户端一个无法修复的报错），但把日志从 info 提到
      **warning** 并明确说明"显示设置未被应用、串流按当前布局继续"，消除"静默假装成功"的含糊。

36. **第十四轮：WebUI HDR 运行时状态（2026-09-11）**：`GET /api/runtime/hdr` 此前在 Linux 恒报
    `available=false`（Windows 由采集端注册状态）。现由 **avcodec 会话**注册同一套共享状态：会话建立时
    按"实际能承载什么"填写（pq/hlg/sdr、分析模式、分析是否活跃、**确实能发**的元数据格式
    hdr10_plus/hdr_vivid），首帧有效统计时置 `scene_metadata_active` 并刷新，析构时注销；SDR 会话不
    注册。`conversion_path` 在 Linux 留空（KMS 直出 PQ/HLG，不做转换），前端因此不显示该行，避免被
    误标为 D3D11 路径。Windows 注册路径与文案未变（Linux 部分整段平台门控）。

37. **第十五轮：拓扑校验一致性与差异决策记录（2026-09-11）**：`is_topology_valid` 现在与自身 setter
    一致地**拒绝多设备组**（此前会"通过校验、再在 setter 里以另一条消息失败"；Windows 允许每组 ≤2，
    Linux 在有合成器侧后端前无法表达镜像组）。另外把 `Sunshine-Virtual-Microphone` 提为具名常量，并把
    复核后**有意保留**的差异写入 `LINUX_PORT_GAPS.md` §5.18：D11 空容器契约（树内无空集合调用方，
    setter 的空 map 语义被还原路径依赖）、A5 麦克风契约边界（改成 Windows 语义会触发无意义的重初始化，
    缓冲属性改动无法在本机验证）、D19 日志文案（Linux 日志统一英文，用户可见 UI 走托盘 i18n 中/英/日）、
    F3' 前台 exe 语义（Wayland 无进程映像名来源）、F9 直方图估计器（Linux 更精确）、D15 复制拓扑
    （**已核实 kscreen-doctor 无 replication 设置命令**，需 KWin 脚本或 libkscreen，属独立特性）。

38. **终局核验与 Windows 影响面审计（2026-09-11）**：对工作区 54 个提交逐项核对"在 Windows 上编译"
    的文件（详见 `LINUX_PORT_GAPS.md` §5.19）：8 个 VDD helper 为逐字搬移（脚本验证）、Windows 采集设备
    恒设置 `data` 故 CPU 分析器相关改动在 Windows 上不可达、硬件下载/Vivid/状态上报整段平台门控、
    `display_vram.cpp` 的三个采样常量数值不变、托盘菜单的 Windows 语句与文案逐字未变、其余共享头均为
    增量新增。**唯一 Windows 可见行为变化**是 HDR10+ 元数据改为首帧有效统计才挂载（消除伪造 SEI，与
    该平台原生路径一致）。同时把 C5（跨语言常量同步）转为决策记录（不引入代码生成，靠线协议测试守
    住 C++ 镜像），并把**仍未完成**的三件事写入 `LINUX_PORT_GAPS.md` §5.20：F4（HLG 域分析源）、
    niri/wlr-output-management 输出后端、D15（复制拓扑），外加 §5.18 的有意保留差异清单。

**测试基线复核（2026-09-11，pkgrel 53 构建树；终局核验：全量重建 + 全套测试通过，见进度 38）**：`ctest` 13 个套件
12 个通过。聚合套件 `test_sunshine` 共 519 个用例：507 通过、12 跳过（1 个 Unicode 路径用例 +
Audio/MouseHID/Encoder 三个环境套件的用例）、**0 个断言失败**；AudioTest / MouseHIDTest /
EncoderTest 仍仅 `SetUpTestSuite` 失败（需真实音频/输入/编码器环境，图形会话内可跑）。
本轮曾暴露并修掉一个真实测试失败：`VddEdid.MatchesReference1080p60Hdr` 的字节参考向量钉的是旧
Range Limits 描述符（写死 preferred±20 → 40–80 Hz），而 `4ad74c90` 起该描述符由通告模式推导
（无附加模式时 1920x1080@60 → 55–65 Hz，差异仅 2 字节 `0x28,0x50`→`0x37,0x41`）——属测试期望未随
新语义更新；向量已更新并在用例内注明与 sunshineVD `generator.py` 的有意偏差。
注：`ctest` 直跑需要可写的 `$HOME`（FileHandler 用例在 `~/.config/sunshine` 下建目录），只读
`$HOME` 会让聚合套件提前 abort，属环境差异而非代码回归；且必须重建 `test_sunshine`
（增量构建只编译 `sunshine` 时，ctest 会跑旧二进制并掩盖新失败）。

**下一个目标（2026-09-11 起）**：**niri 支持**（`LINUX_PORT_GAPS.md` §2.11：前台检测 producer、
通用输出后端 wlr-output-management、niri 输出控制）、剪贴板补图片类帧与大文件 blob 回退
（§1.4/§2.2）；其后是 display_device 的失败处理与顺序补齐（§5.2 D5/D7/D8/D9/D13/D14）、麦克风
背压契约（A1）、亮度分析器覆盖面（VAAPI/CUDA，F1）。HDR 三件套（HDR10+ / DV P8.1 / Vivid）与
虚拟屏完整模式表两项已收官。

**标签**：`v0.1-linux-base`（虚拟屏工作开始前的基线）→ `v0.2-linux-vdd`（虚拟显示器原生后端完成，
随 `48b7c4bf`）→ `v0.3-linux-display-device`（物理显示器后端）→ `v0.4-linux-clipboard-host`
（主机侧剪贴板）→ `v0.5-linux-mic-redirect`（远程麦克风）→ `v0.5.1-linux-restart-fixes` /
`v0.5.2-linux-tray-restart`（重启两轮修复）→ `v0.6-linux-abr-foreground`（ABR 前台检测）→
`v0.7-linux-hdr10plus`（HDR10+ 亮度分析器）→ `v0.8-linux-dv-rpu`（DV P8.1 RPU）→
`v0.8.1-linux-tray-vdd-state` / `v0.8.2-linux-tray-msgbox`（托盘 VDD 状态、消息框）→
`v0.8.3-linux-manual-vdd` / `v0.8.4-linux-vdd-mode-list`（手动模式参数化）→
`v0.8.5-linux-vdd-name` / `v0.8.6-linux-parity-audit`（命名与边界对齐）→
`v0.8.7-linux-vdd-refresh-ladder`（刷新率阶梯）→ `v0.9.0-linux-full-mode-table`（链式 CTA 全模式表）→
`v0.9.1-linux-full-cross-product`（**当前**，pkgrel 38）。此后每完成一个功能里程碑继续打 tag。

**增强迁移（§十一）**：P0 虚拟显示器已完成（见进度 7）。后续按 物理 display_device 后端 →
P1（剪贴板主机侧、远程麦克风）→ P2（HDR 动态元数据、ABR 前台检测）→ P3 的顺序推进；
每步独立成 commit，里程碑处打 tag，便于对照上游 rebase。

---

### 附：本报告关键证据文件

- 打包模板缺失：`cmake/prep/special_package_configuration.cmake:11-45`、`packaging/linux/`（仅剩 flatpak/deps）
- Linux 编译开关与依赖探测：`cmake/compile_definitions/linux.cmake`、`cmake/prep/options.cmake`
- 预编译 FFmpeg：`cmake/dependencies/common.cmake:31-83`、`third-party/build-deps/dist/Linux-x86_64/`
- 托盘强制依赖：`cmake/compile_definitions/linux.cmake:177-208`
- CI 仅 Windows：`.github/workflows/main.yml`（jobs: setup_release / vdd_smoke / build_win）
- CUDA 架构推导问题：`cmake/compile_definitions/linux.cmake:22-69`
- 分支功能平台守卫：`src/abr.cpp:86-132`、`src/platform/linux/audio.cpp:543-556`、
  `src/remote_usb/remote_usb_host_controller.cpp:71-82`、`cmake/packaging/FetchGUI.cmake:24`
- VDD 会话逻辑（跨平台）与 Windows 后端：`src/display_device/session.cpp:143-293`、
  `src/display_device/vdd_utils.cpp`、`src/display_device/vdd_ioctl.cpp:9-21`（无守卫 Windows.h，
  Linux 编译首个报错点）；配置键 `vdd_keep_enabled`/`vdd_reuse`/`vdd_headless_create`（`src/config.cpp`）
- sunshine-virt-display（本机 `/opt/sunshine-vd/`，frostplexx/sunshine_virt_display）：
  socket 协议与 Sunshine prep 命令集成见其 `readme.md`，KWin 集成 `src/drm/de/kwin.py`，
  守护进程 `src/daemon/daemon.py`（`/tmp/sunshineVD.sock`，`sunshineVD.service`）；
  C++ 原生移植参考 `src/display.py`（流程）、`src/edid/generator.py:185-188`（EDID 含 HDR 块）、
  `src/drm/drm_master.py`（pidfd 借 master）、`src/drm/crtc.py`（CRTC 强制指派）
- 剪贴板可插拔 sink：`src/clipboard_bridge.h:46`（`inbound_sink_fn`）
- NVENC 直连的 OS 中立层：`src/nvenc/common_impl/`（工厂 `src/nvenc/win/` 为 D3D11，Linux 需新建 CUDA 工厂）
- 复查轮补充：DRED `src/audio.cpp:398-403`；DV RPU `src/video_dolby_vision.h:2-21`、
  `video.cpp:3069-3100`、亮度输入缺口 `display_vram.cpp:2341-2422`；HDR 静态透传
  `kmsgrab.cpp:850-862` + `video.cpp:2839-2862`；HLG 捕获限制 `kmsgrab.cpp:840-841`；Vivid
  序列化缺口 `video.cpp:2925-2937`；探测缓存 `video.cpp:4534-4537`；托盘菜单门控
  `system_tray.cpp:1095-1099`；Linux display_device 全 stub `src/platform/linux/display_device.cpp`
- 控制面板子模块（`src_assets/common/sunshine-control-panel`，本机工作树曾处于空置状态，
  已用 `git checkout HEAD -- .` 恢复）：依赖分层 `src-tauri/Cargo.toml`
  （`[target.'cfg(windows)'.dependencies]`），模块清单 `src-tauri/src/main.rs:4-47`
- 虚拟屏模式表与 EDID 链式 CTA 块：`src/platform/linux/vdd_edid.h`（`edid_options.extra_modes`、
  `mode_fits_pixel_clock_limit`）、`vdd_edid.cpp:54-63`（空白模型单一来源）、`:244-253`（Range Limits
  覆盖全部通告刷新率）、`:256-275`（块 1 布局、链式块数与逐块校验和）；配置列表推导与可行性过滤
  `src/display_device/vdd_utils.cpp:1835-1906`（阶梯 = 全组合叉积）、`:1916-1962`（会话外偏好模式）、
  `:1764+`（`adopt_orphan_vdd_locked`，从 EDID DTD 解析回偏好模式）
- 托盘 VDD 状态与消息框：`src/tray/system_tray.cpp:87-101`（`show_message_box`，QMessageBox）、
  `:1358-1378`（2s 状态轮询线程）、`:1306-1311`（`checkbox=1` 渲染勾选态）；
  构建面 `cmake/compile_definitions/linux.cmake:200-208`（pkg-config 解析 Qt6Widgets）
- Windows parity 规则沉淀：`AGENTS.md`（共享常量 / 语义含边界 / 用户可见命名）；EDID 单元测试
  `tests/unit/test_vdd_edid.cpp`（7 用例），Windows 侧排除 `tests/CMakeLists.txt:307-313`
- 亮度分析器与 DV RPU（avcodec 路径）：`src/video.cpp:311`（`analyze_pq_luma_frame`）、
  `:473`（convert 中调用）、`:2440-2473`（stage L1 + 按 pts splice RPU NAL）、`:3203` 起（sessions
  的 `dolby_vision_.configure` 门控）；ST2084 常量 `src/video_hdr_metadata.h:24-28`；Vivid 序列化缺口
  `video.cpp:3091-3105`
