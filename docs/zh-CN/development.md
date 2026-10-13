# 开发文档

[项目首页](../../README.md) · [文档目录](README.md) · [English](../en/development.md)

从要修改的模块选择入口。Foundation 的主机服务、浏览器 WebUI 和 Windows 桌面 GUI 分别有自己的运行环境；可选设备组件还有独立的安装与会话生命周期。

## 代码结构

| 模块 | 入口 | 负责的内容 |
| --- | --- | --- |
| 主机服务 | `src/` | 捕获、编码、音频、输入、配对与会话 |
| 客户端扩展 | `src/nvhttp/`、`src/rtsp.cpp`、`src/stream.cpp` | 协商、控制通道、动态参数、显示控制与网络反馈 |
| 视频与 HDR | `src/video*`、`src/hdr/`、`src/platform/windows/display*` | 色彩、元数据、显示目标和捕获路径 |
| 网络自适应 | `src/streaming/` | 码率与丢包保护 |
| 浏览器 WebUI | `src_assets/common/assets/web/` | 管理界面、配置、应用列表与网页诊断 |
| 桌面 GUI | `src_assets/common/sunshine-control-panel/` | 设备中心、本地扫描、组件与系统操作；独立仓库子模块 |
| 设备组件 | `src/platform/windows/ds5/`、`src/remote_usb/` 等 | 可选设备、传输与进程生命周期 |
| 自动化检查 | `tests/`、WebUI `tests/`、`.github/workflows/` | 单元、集成与构建检查 |

## 获取源码

```bash
git clone --recurse-submodules https://github.com/AlkaidLab/foundation-sunshine.git
cd foundation-sunshine
```

已有工作目录时，用 `git submodule update --init --recursive` 获取记录的子模块版本。主程序与 GUI 协同修改时，分别记录两个仓库的提交和使用的构建包。

## 主机服务构建

依赖和平台步骤见[构建说明](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/building.md)。Windows 官方打包流程见 [Build and Release](https://github.com/AlkaidLab/foundation-sunshine/blob/master/.github/workflows/main.yml)；构建文档中的通用示例与官方安装包路径并不完全相同。

修改可选组件时同时检查包来源、版本、运行能力和回退路径：

- [DualSense 组件生命周期](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/windows_dualsense_component_lifecycle.md)
- [NVIDIA HDR 构建](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/rtx_hdr_build.md)
- [主程序与 GUI 配对构建](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/paired-gui-build.md)

## WebUI 开发

使用主仓库 `.node-version` 指定的 Node.js 和 `package.json` 指定的 npm 版本，在仓库根目录执行：

```bash
npm ci
npm run dev-server
```

开发服务器使用 `https://localhost:3000`，接口需要可访问的 Sunshine 实例。修改后运行与该界面相关的检查：

```bash
npm run lint:webui
npm run test:webui
npm run build
```

语言变更还需检查 `npm run i18n:validate`。完整说明见 [WebUI 开发指南](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/WEBUI_DEVELOPMENT.md)。

## 桌面 GUI 开发

GUI 位于子模块，对应 [sunshine-control-panel](https://github.com/qiin2333/sunshine-control-panel)。在该目录按其依赖要求安装 Node.js、Rust 与 Windows 原生构建工具，再执行：

```bash
npm ci
npm run dev
```

`dev` 会构建原生插件并启动 Tauri；纯界面开发可用 `npm run dev:renderer`。主仓库 WebUI 与 GUI 联调时，先在主仓库启动 `npm run dev-server`，再在 GUI 目录运行 `npm run dev-webui`。

常规 Windows 构建下载可用的 GUI release 包。需要验证主程序和固定 GUI 提交的组合时，使用[配对构建说明](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/paired-gui-build.md)，避免把工作目录子模块版本当成发行包内 GUI 的版本。

## 如何验证修改

| 修改类型 | 应确认的结果 |
| --- | --- |
| 捕获或编码 | 实际连接的捕获方法、编码器、格式和异常回退；必要时比较帧率与延迟 |
| HDR 与色彩 | 协商、主机信息注入、控制端解码和显示效果分别核对 |
| 音频或输入 | 真实设备录音、播放或按键反馈，确认断线后释放状态 |
| 设备组件 | 安装、不可用、修复、卸载与会话结束后的清理 |
| WebUI 或 GUI | 实际操作流程，浏览器与桌面环境差异，中英文显示 |
| 文档 | 相对链接、章节锚点、双语导航和用户照步骤能否完成任务 |

没有运行的检查写明原因。能通过单元测试不等于已经验证客户端、驱动或硬件。PR 内容要求见[贡献指南](../contributing.md)。
