# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

本文件为 Claude Code (claude.ai/code) 在处理此仓库代码时提供指导。

## 项目概述

Foundation Sunshine 是一个为 Moonlight 客户端提供的自托管游戏串流主机，从 LizardByte/Sunshine 分叉而来。此分支专注于增强的 HDR 支持（HDR10/PQ 和 HLG）、集成的虚拟显示管理、远程麦克风支持，以及为各种客户端设备优化的串流体验。

**核心技术：**
- C++17 后端，包含平台特定实现（Windows、macOS、Linux）
- Vue 3 + Vite 前端（Composition API）
- CMake 构建系统（需要 3.20+）
- FFmpeg 用于视频/音频编码
- 硬件编码支持：NVENC（Nvidia）、AMF（AMD）、QSV（Intel）、VideoToolbox（macOS）

## 构建命令

### 初始设置
```bash
# 克隆并包含子模块
git clone --recurse-submodules <repo-url>
cd foundation-sunshine

# 安装依赖（平台特定依赖请参见 docs/building.md）
# macOS: brew install boost cmake miniupnpc node openssl@3 opus pkg-config
# Windows: 使用 MSYS2 UCRT64（参见 docs/building.md）
# Linux: 参见 scripts/linux_build.sh 获取发行版特定的包
```

### 构建 C++ 后端
```bash
# 配置和构建
cmake -B build -G Ninja -S .
ninja -C build

# 构建并包含测试
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON
ninja -C build
```

### 构建 WebUI（前端）
```bash
# 安装 npm 依赖
npm install

# 开发模式（监听变更）
npm run dev

# 开发服务器（HTTPS 和热重载）
npm run dev-server

# 生产构建
npm run build

# 构建并预览生产版本
npm run preview:build
```

### 运行测试
```bash
# 首先构建测试
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON
ninja -C build

# 运行所有测试
./build/tests/test_sunshine

# 运行特定测试
./build/tests/test_sunshine --gtest_filter=TestName.*
```

### 打包
```bash
# macOS
cpack -G DragNDrop --config ./build/CPackConfig.cmake

# Windows (MSYS2)
cpack -G NSIS --config ./build/CPackConfig.cmake  # 安装程序
cpack -G ZIP --config ./build/CPackConfig.cmake   # 便携版

# Linux
cpack -G DEB --config ./build/CPackConfig.cmake   # Debian/Ubuntu
cpack -G RPM --config ./build/CPackConfig.cmake   # Fedora/RHEL
```

## 架构

### 核心组件

**视频管道 (src/video.cpp, src/video.h)**
- 处理视频捕获、编码和串流
- 支持动态参数调整（分辨率、FPS、比特率、QP）
- HDR 支持：HDR10（PQ/ST2084）和 HLG（BT.2100），带动态元数据生成
- 逐帧亮度分析（MaxFALL/MaxCLL 计算）
- NVENC、AMF、QSV、VideoToolbox 的硬件编码器抽象

**显示设备管理 (src/display_device/)**
- 虚拟显示创建和管理（Windows 10 22H2+）
- 显示配置解析和会话管理
- 平台特定实现位于 src/platform/*/display_device/

**音频管道 (src/audio.cpp, src/audio.h)**
- 音频捕获和编码
- 远程麦克风支持（接收客户端音频）
- 平台特定音频后端

**输入处理 (src/input.cpp, src/input.h)**
- 键盘、鼠标、游戏手柄、触摸和笔输入
- 通过 src/platform/*/input/ 进行平台特定输入注入

**网络层**
- NVHTTP 协议 (src/nvhttp.cpp)：GameStream 协议实现
- RTSP 串流 (src/rtsp.cpp)：实时串流协议
- Config HTTP 服务器 (src/confighttp.cpp)：Web UI 后端 API
- UPnP 支持 (src/upnp.cpp)：自动端口转发

**进程管理 (src/process.cpp)**
- 应用程序启动和生命周期管理
- 会话处理和客户端配对

### 平台抽象

平台特定代码隔离在 `src/platform/{windows,macos,linux}/`：
- **显示捕获**：X11、Wayland、KMS/DRM（Linux）、DXGI（Windows）、AVFoundation（macOS）
- **输入注入**：Inputtino（Linux）、Windows API（Windows）、CGEvent（macOS）
- **音频**：PulseAudio/PipeWire（Linux）、WASAPI（Windows）、AVFoundation（macOS）
- **硬件编码**：NVENC/CUDA、VAAPI（Linux）、D3D11（Windows）、VideoToolbox（macOS）

### WebUI 架构

**前端结构** (src_assets/common/assets/web/)：
- **views/**：页面级组件（Home.vue、Apps.vue、Config.vue 等）
- **components/**：可复用 UI 组件
  - layout/：Navbar、PlatformLayout
  - common/：ThemeToggle、ResourceCard、VersionCard、ErrorLogs
- **composables/**：业务逻辑（useVersion、useApps、useConfig、useLogs 等）
- **services/**：API 服务层（appService.js）
- **config/**：Firebase 分析、i18n 配置
- **utils/**：辅助函数、验证、主题管理

**关键模式：**
- 所有页面使用 Vue 3 Composition API（`<script setup>`）
- 业务逻辑提取到 composables 以实现可复用性
- 通过 vue-i18n 实现国际化（模板中使用 `$t()`，脚本中使用 `useI18n()`）
- 使用 Bootstrap 5 进行样式设计

## 开发工作流

### 添加新功能

**后端（C++）：**
1. 确定适当的模块（video、audio、input、network 等）
2. 在 src/ 头文件中添加平台无关接口
3. 在 src/platform/{windows,macos,linux}/ 中实现平台特定代码
4. 如果添加新文件，更新 CMakeLists.txt
5. 在 tests/unit/ 中添加单元测试

**前端（WebUI）：**
1. 在 composables/ 中创建业务逻辑的 composable
2. 在 components/ 中创建组件或在 views/ 中创建页面
3. 将 i18n 键添加到 src_assets/common/assets/web/public/assets/locale/en.json
4. 运行 `npm run i18n:sync` 将键传播到其他语言
5. 运行 `npm run i18n:format` 格式化翻译文件
6. 如果添加新页面，创建 HTML 入口文件
7. 使用 `npm run dev-server` 进行测试

### 处理 HDR 视频

HDR 管道位于 src/video.cpp 和 src/platform/*/av_video.*：

**双格式支持：HDR10 (PQ) + HLG**
- HDR10（PQ）：使用 ST2084 传输函数和静态元数据，绝对亮度映射
- HLG：使用混合对数伽马（ITU-R BT.2100），场景参考式相对亮度映射
  - 优势：低亮度设备上暗部细节保留更好，高光区域平滑滚降，天然 SDR 向后兼容

**逐帧亮度分析**
- GPU Compute Shader 实时计算 MaxFALL/MaxCLL
- 百分位截断策略过滤极端亮度像素
- 帧间指数平滑（EMA）消除亮度闪烁
- 动态注入 HEVC/AV1 SEI/OBU 元数据

**元数据透传**
- HDR10 静态元数据（Mastering Display Info + Content Light Level）
- HDR Vivid 动态元数据
- HLG 传输特性标识
- 符合 CTA-861 规范的完整色彩容积与亮度信息

编码器特定实现位于平台目录中。

### 修改显示设备逻辑

虚拟显示管理位于 src/display_device/：
- parsed_config.cpp：配置解析
- session.cpp：会话生命周期管理
- 平台实现：src/platform/windows/display_device/（Windows 特定）

### i18n 工作流

```bash
# 验证翻译完整性
npm run i18n:validate

# 从 en.json 同步缺失的键到其他语言
npm run i18n:sync

# 格式化所有翻译文件
npm run i18n:format

# 检查格式但不做更改
npm run i18n:format:check
```

## 重要构建选项

通过 `cmake -B build -D<OPTION>=ON/OFF` 配置：
- `BUILD_TESTS`：启用单元测试（默认：OFF）
- `BUILD_DOCS`：构建 Doxygen 文档（默认：OFF）
- `SUNSHINE_ENABLE_TRAY`：系统托盘图标（默认：ON，macOS 上忽略）
- `SUNSHINE_ENABLE_CUDA`：CUDA/NVENC 支持（仅 Linux，默认：ON）
- `SUNSHINE_ENABLE_WAYLAND`：Wayland 捕获（仅 Linux，默认：ON）
- `SUNSHINE_ENABLE_X11`：X11 捕获（仅 Linux，默认：ON）
- `BUILD_WERROR`：将警告视为错误（默认：OFF）

## 代码模式

### 动态参数调整
视频编码参数可以通过 `dynamic_param_t` 事件在串流中途更改（src/video.h）：
- 分辨率、FPS、比特率、QP、FEC 百分比
- 预设、自适应量化、多遍编码
- VBV 缓冲区大小

### 平台特定代码
使用预处理器保护：
```cpp
#ifdef _WIN32
  // Windows 特定
#elif __APPLE__
  // macOS 特定
#elif __linux__
  // Linux 特定
#endif
```

### 日志记录
使用 Boost.Log 宏：
```cpp
BOOST_LOG(info) << "Message";
BOOST_LOG(warning) << "Warning";
BOOST_LOG(error) << "Error";
BOOST_LOG(fatal) << "Fatal error";
```

### 线程安全
使用 src/thread_safe.h 中的安全包装器：
- `safe::mail_t`：线程安全消息队列
- `safe::event_t`：线程安全事件信号
- `safe::queue_t`：线程安全队列

## 测试

测试使用 Google Test 框架（tests/unit/）：
- test_video.cpp：视频编码测试
- test_audio.cpp：音频捕获测试
- test_network.cpp：网络协议测试
- test_mouse.cpp：输入处理测试
- 平台特定测试位于 tests/unit/platform/

## 文档

- **构建**：docs/building.md
- **配置**：docs/configuration.md
- **WebUI 开发**：docs/WEBUI_DEVELOPMENT.md
- **性能调优**：docs/performance_tuning.md
- **故障排除**：docs/troubleshooting.md

## 平台特定说明

### macOS
- 使用 Homebrew 或 MacPorts 安装依赖
- OpenSSL 可能需要手动链接：`ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl`
- VideoToolbox 是主要的硬件编码器
- AVFoundation 处理显示/音频捕获
- 虚拟显示功能不可用（仅限 Windows 10 22H2+）

#### macOS 构建警告

**关键提示**：在 macOS 上构建时，以下情况必须执行完全清理构建：
- 从 git 拉取更新后
- 修改 CMake 配置文件后
- 切换分支后
- 安装/更新 Homebrew 包（特别是 FFmpeg）后

**原因**：macOS 构建使用捆绑的 FFmpeg 静态库。如果 CMake 缓存过时，系统头文件（Homebrew）可能会干扰，导致运行时崩溃。

**清理构建命令**：
```bash
rm -rf build
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

**测试验证**：构建后验证编码器工作正常：
```bash
./build/sunshine 2>&1 | grep -i "encoder.*passed"
```

预期输出：`Info: Found H.264 encoder: h264_videotoolbox [videotoolbox]`

**故障排除**：如果遇到崩溃或编码器失败，请参阅 `docs/troubleshooting.md` → "macOS-Specific Issues"

### Windows
- 使用 MSYS2 UCRT64 环境进行构建
- 支持虚拟显示器管理（需要 Windows 10 22H2 或更高版本）
- NVENC、AMF、QSV 硬件编码支持
- DXGI 用于显示捕获

### Linux
- 支持 X11、Wayland、KMS/DRM 显示捕获
- VAAPI、NVENC/CUDA 硬件编码
- PulseAudio/PipeWire 音频后端
- Inputtino 用于输入注入

## 提交规范

查看最近的提交历史以了解项目的提交消息风格：
```bash
git log --oneline -10
```

常见提交前缀：
- `feat:` - 新功能
- `fix:` - 错误修复
- `docs:` - 文档更新
- `refactor:` - 代码重构
- `test:` - 测试相关
