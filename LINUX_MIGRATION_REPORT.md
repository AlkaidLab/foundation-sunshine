# Foundation Sunshine Linux（Arch Linux）迁移报告

- **日期**：2026-09-08
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

此外要建立正确的预期：本分支的卖点（ZakoVDD 虚拟显示器、NVENC SDK 13 直连 / AMF QVBR、Tauri 控制
面板、远程麦克风写主机、USB/IP 主机、WGC 捕获、vmouse 虚拟鼠标）**在 Linux 上均不可用**（Windows
专属或不完整 stub）。Linux 上能得到的是"上游 Sunshine 功能集 + 分支的部分纯逻辑增强"
（见 §八功能矩阵）。CI 目前只有 Windows 构建（`.github/workflows/main.yml` 仅有 `build_win`、
`vdd_smoke` 两个 Windows job），Linux 编译在主干上长期无人验证，首次构建可能遇到零星的编译错误。

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
| 打包模板 | **全部缺失**（见 §三） |

---

## 三、硬阻塞：Linux 打包模板缺失（必须先修）

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
| Webhook / client_fingerprint / launch_session_manager / ABR* | 纯逻辑层，均有 Linux 分支（`*` ABR 的前台应用检测仅 Windows，Linux 返回空 → 相关自适应决策失效，`src/abr.cpp:86-132`） |
| 剪贴板（客户端↔客户端 / 客户端↔WebUI 中继） | 内存中继 + SSE 跨平台；**无主机 OS 剪贴板同步**（全库没有 X11/Wayland 剪贴板集成代码，宿主侧 provider 本是 Windows GUI 面板） |
| AI API Key 凭据 | Linux 降级为明文环境变量 `SUNSHINE_LLM_API_KEY`（Windows 用 DPAPI，`src/ai/credential_store.cpp:117-124`） |
| 音频增强：Opus DRED / 持续音频 / 7.1.4 12 声道 | DRED 是 libopus≥1.5 的编译期特性检测（`src/audio.cpp:398-403`，Arch opus 1.6.1 ✅）；12ch 有 Linux null-sink 实现（`platform/linux/audio.cpp:406-408`）；持续音频为平台无关逻辑 |
| HDR 静态元数据透传（MDCV/CLL） | 跨平台：kmsgrab 读 DRM `HDR_OUTPUT_METADATA`（`kmsgrab.cpp:850-862`）→ avcodec side data（`video.cpp:2839-2862`），不依赖编码器 SDK |
| 编码器探测缓存（README"260x"） | 位于 `video.cpp` 探测层的跨平台缓存（`video.cpp:4534-4537`）；注意实现是内存缓存，README 的"持久化"表述与代码不符 |
| 增强托盘（fork 新增，上游无 src/tray） | 打开 UI/语言/项目链接/重启/退出 + 通知，Linux 可用；仅 VDD 子菜单与高级设置菜单在 `#ifdef _WIN32` 内（`system_tray.cpp:1095-1099`） |
| nvhttp 扩展 API：dynamic_params / network_probe / sessions / abr_api / ai_api / pairing | 纯 HTTP 协议层，零平台守卫，跨平台 |
| perf_recorder / input_activity / video_probe / cursor_channel | 全部跨平台；cursor_channel 在 Linux 因无光标生产者而干净禁用（`stream.cpp:2270-2273` 拒绝并告警，不影响流） |

### ⚠️ 降级可用（编译运行正常，功能面收窄）

| 功能 | Linux 状态 |
|---|---|
| Dolby Vision P8.1 / P8.4 | RPU 写入器是纯比特流层（`video_dolby_vision.h:2-21`，0 平台守卫），挂在通用 avcodec 编码路径（`video.cpp:3069-3100`）；但 L1 亮度分析输入仅 Windows NVAPI 产出（`display_vram.cpp:2341-2422`），Linux 无 stats → 不生成 RPU，会话退化为普通 HDR10（基层兼容层保证回退，不崩） |
| HDR10+ 动态元数据 | Linux 只发预挂的占位 SEI（`video.cpp:2867+`），真实值刷新依赖 Windows-only 亮度分析器（`video.cpp:2288-2296`） |
| display_control / display_scale API | HTTP 层可用；Linux 返回 unsupported 字段，VDD 能力协商明确报 `unsupported_platform`（`vdd_capability.cpp:10-12`） |
| frame_contract（帧管线契约） | 策略层跨平台（`platform/frame_contract.cpp`），但只有 Windows 采集端消费，Linux 侧策略存在、执行为空 |

### ❌ Windows 专属 / Linux 不可用

| 功能 | 原因 |
|---|---|
| ZakoVDD 虚拟显示器（含 5 种屏幕模式、零拷贝借帧） | 仅 `src/platform/windows/`，Linux 不编译 |
| NVENC SDK 13 直连 / AMF QVBR / 多硬件实例 | `src/nvenc/`、`src/amf/` 仅进 Windows 构建（`compile_definitions/windows.cmake:114-120`）；Linux NVENC 为上游同款 FFmpeg 路径（注：探测缓存是跨平台的，见 ✅ 表） |
| HLG 编码（Linux 侧） | 会话框架跨平台，但 kmsgrab 不支持 HLG EOTF 输入（`kmsgrab.cpp:840-841`）→ Linux 无原生 HLG 源 |
| HDR Vivid 动态元数据 | avcodec 路径没有 CUVA T.35 序列化器（`video.cpp:2925-2937` 注释明示），仅 Windows NVENC 直连路径手写产出 |
| 虚拟扬声器位深匹配 | Windows PolicyConfig COM（`platform/windows/audio.cpp:1316-1319`）；Linux 固定 `PA_SAMPLE_FLOAT32`（`platform/linux/audio.cpp:81`） |
| 触摸键盘自动唤起（touch_keyboard_session） | Windows 注册表机制，头文件自述非 Windows 为 no-op（`touch_keyboard_session.h:11-12`） |
| Linux 分辨率/HDR 运行时切换 | `src/platform/linux/display_device.cpp` 全 stub（119 行）——上游同款状态；虚拟屏工作流依赖 §十一 的 VDD 移植 |
| Tauri 控制面板 | 分发链 Windows 化（`FetchGUI.cmake:24`）；面板本体评估见 §十一 P3 |
| 远程麦克风写主机 | `src/platform/linux/audio.cpp:543-556` 是返回 -1 的空实现（"not implemented on Linux yet"） |
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

### P0 · 虚拟显示器 —— 已装 `sunshine-virt-display`，可先行落地

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

### P1 · 远程麦克风写主机（补一个 stub）

钩子位置现成：`src/platform/linux/audio.cpp:543-556` 的 `write_mic_pcm()` 返回 -1 空实现，注释明说
"not implemented on Linux yet"。实现：经 PipeWire 建虚拟麦克风源（pw_stream / `module-loopback` /
null-sink monitor），把混音后的 PCM 写入。逻辑层（Opus 解码/混音）已跨平台在跑。工作量：中。

### P1 · 剪贴板主机侧集成（体验提升最大的一项）

分支剪贴板是"内存中继 + 可插拔 sink"架构（`src/clipboard_bridge.h:46` 的 `inbound_sink_fn`），目前
Linux 只有客户端↔客户端/浏览器中继，缺主机 OS 读写。实现两个 provider 即可接入现有总线：
X11（XFixes selection 事件）+ Wayland（wlr-data-control 协议，KWin 支持，即 `wl-clipboard` 依赖的
机制）。工作量：中。

### P2 · HDR 动态元数据与 DV 亮度分析（复查后的修订版）

静态透传已确认跨平台可用（见 §八 ✅ 表），无需移植。动态元数据在 Linux 有**三个缺口**，均已有
明确的移植路径：

1. **逐帧亮度分析器**（HDR10+ 真实刷新与 DV L1 RPU 的共同前置）：Windows 用 D3D11 Compute
   Shader/NVAPI（`display_vram.cpp:2341-2422`）。Linux 可用 CUDA kernel（nvenc 会话本就有 CUDA
   hwdevice）或 VA-API/Vulkan compute 实现，产出到 `platf::hdr_frame_luminance_stats_t` 即可同时
   点亮 HDR10+ 刷新与 DV RPU。工作量：高（但收益覆盖两个特性）。
2. **HDR Vivid T.35 序列化器**：avcodec 路径没有 CUVA 序列化器，但分支已有自研比特流工具层
   （`src/cbs.cpp`/`video_hdr_bitstream.cpp`，DV RPU 写入器就是同模式自研的），照搬 `nvenc_base.cpp`
   的手写 T.35 逻辑到 cbs 层即可。工作量：中。
3. **HLG 捕获源**：kmsgrab 不接受 HLG EOTF（`kmsgrab.cpp:840-841`），属上游内核/DRM blob 能力
   限制（`HDR_OUTPUT_METADATA` 仅定义 PQ/SDR），短期放弃；P8.4（HLG 基层）随 HLG 一起搁置。

### P2 · ABR 前台应用检测

`src/abr.cpp:86-132` 的 `detect_foreground_app()` 仅 Windows。Linux 实现：KWin Scripting/D-Bus
或 wlr-foreign-toplevel-management（KWin 均支持）。工作量：低-中。

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
   剩余：第 5 步 `sudo ninja install` + setcap/udev，然后第 6 步实测串流。

**增强迁移（§十一）**：基础跑通后按 P0（虚拟显示器：先路线 A 零代码验证，再路线 B 代码级）→
P1（远程麦克风、剪贴板主机侧）→ P2（HDR 注入验证、ABR 前台检测）→ P3 的顺序推进；
每步独立成 commit，便于对照上游 rebase。

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
