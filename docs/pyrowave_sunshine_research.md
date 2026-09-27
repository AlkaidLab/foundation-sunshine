# Pyrowave 与 Foundation Sunshine 集成研究报告

日期：2026-09-27

## 1. 结论

Pyrowave 适合在主机与客户端位于同一有线千兆网络、且用户优先考虑端到端延迟的场景中作为实验性视频编码方案。它的主要优势来自 GPU Compute 编解码和帧内编码，代价是带宽显著高于 H.264、HEVC 和 AV1。

将 Pyrowave 集成到 Foundation Sunshine 需要同时扩展 Sunshine 服务端、Moonlight 客户端、视频格式协商、RTP/帧分片和 Vulkan 图像互操作。当前需求不能按“增加一个编码器名称”处理。

建议先保留为增强需求，使用单一客户端做独立 PoC，默认关闭，并保留 H.264、HEVC 和 AV1 回退。待 Pyrowave 的 API/ABI 和客户端协议稳定后，再评估是否进入正式版本。

## 2. 需求背景

本报告评估在 Sunshine 中增加 Pyrowave codec 的技术可行性。当前没有具体客户端、GPU、操作系统或性能数据，因此只能评估技术可行性，不能判断对现有 Foundation Sunshine 用户的实际收益。

Steam 已在 2026 年 9 月的 Beta 客户端中加入 Pyrowave 实验支持。官方说明支持 Windows 和 macOS，Linux 需要实验性的 SteamRT3 客户端，移动端支持后续提供。Steam 将其定位为高带宽、低延迟选项，并建议主机和客户端使用至少千兆以太网：

- [Steam Remote Play：Pyrowave video codec now in beta](https://steamcommunity.com/groups/homestream/discussions/0/564794422009744473?snr=2___)
- [Steam Client Beta 更新记录](https://steamcommunity.com/groups/SteamClientBeta/announcements?client_view=1)

截至 2026 年 9 月 25 日，Steam 仍在修复 Pyrowave 的闪烁、GPU 内存泄漏、Compute Queue 竞争和编码延迟问题，说明其仍处于快速迭代阶段。

## 3. Pyrowave 的技术特点

Pyrowave 官方项目的说明如下：

- 帧内编码，实际使用方式接近高速静态图像编码；
- 使用 CDF 9/7 小波变换和 Vulkan Compute Shader；
- 目标码率约 200 Mbps 以上；
- 支持 YCbCr 4:2:0 和 4:4:4；
- 每个 64×64 区块独立编码，丢包后不会像传统参考帧编码一样持续污染后续画面；
- 项目声称 1080p 编解码耗时约低于 0.1 ms，4K 约低于 0.2 ms；
- 当前 C API/ABI 仍处于 0.x 版本，项目明确说明尚未稳定。

资料：

- [Themaister/pyrowave](https://github.com/Themaister/pyrowave)
- [Pyrowave C API](https://raw.githubusercontent.com/Themaister/pyrowave/master/pyrowave.h)
- [Pyrowave CMake 构建要求](https://raw.githubusercontent.com/Themaister/pyrowave/master/CMakeLists.txt)

Steam 文档给出的客户端码率范围为 100～500 Mbps，并说明总延迟仍由编码、网络、解码和显示共同决定。网络本身已经占据主要延迟时，替换编码器未必能带来相同幅度的端到端收益。

## 4. Sunshine 当前视频链路

### 4.1 协议和编码器

Foundation Sunshine 当前 Moonlight 协议中的视频格式主要是 H.264、HEVC 和 AV1：[Limelight.h](../third-party/moonlight-common-c/src/Limelight.h:305)。

当前视频编码器抽象为 H.264、HEVC 和 AV1 三组配置：[video.h](../src/video.h:393)。RTSP 描述阶段也只根据 HEVC 和 AV1 能力广播对应字段：[rtsp.cpp](../src/rtsp.cpp:1238)。

因此，增加 Pyrowave 至少需要新增：

1. 客户端和服务端共同理解的视频格式标识与能力协商；
2. RTSP/控制流中的 Pyrowave 参数，包括尺寸、帧率、色彩空间和目标码率；
3. Pyrowave 帧的 RTP 分片、重组、时间戳和丢包处理；
4. 客户端 GPU 解码、帧同步和显示路径；
5. 不支持 Pyrowave 时的 H.264、HEVC 或 AV1 回退。

### 4.2 Windows 捕获资源

Windows 的 VRAM 捕获路径使用 D3D11 资源。捕获图像类型 `img_d3d_t` 持有 D3D11 纹理、共享句柄和 Keyed Mutex：[display_vram_internal.h](../src/platform/windows/display_vram_internal.h:19)。捕获纹理创建时还会设置 `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` 和 `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`：[display_vram.cpp](../src/platform/windows/display_vram.cpp:4336)。

这意味着 Sunshine 已经可以在 GPU 内部保留捕获画面，通常不需要先复制到 CPU 内存。问题在于这张画面当前是 D3D11 `ID3D11Texture2D`，而 Pyrowave 需要 Vulkan `VkImage` 和 Vulkan 设备。

### 4.3 Vulkan 互操作

当前代码虽然存在 Vulkan 编码器路径，但其 FFmpeg Vulkan 输入设备会单独创建 Vulkan 设备：[video.cpp](../src/video.cpp:4933)。这不能直接证明捕获得到的 D3D11 纹理已经可以被 Pyrowave 使用。

Windows 上可行的低延迟路径是：

```text
D3D11 捕获纹理
    ↓ 共享 NT Handle / 外部内存导入
Vulkan VkImage
    ↓ 外部 Semaphore 或其他同步机制
Pyrowave Vulkan Encoder
```

这个路径需要验证：

- D3D11 资源是否能被当前 GPU 驱动导入 Vulkan；
- D3D11 Keyed Mutex 与 Vulkan 外部同步对象如何对应；
- 捕获 GPU 与 Vulkan 编码 GPU 是否为同一适配器；
- BGRA、FP16、NV12、P010 等格式如何转换；
- VDD、混合显卡和显示器切换时资源是否仍然有效。

单 GPU 且捕获和编码使用同一适配器时，零拷贝方案具有较高可行性。混合显卡场景需要跨适配器复制或共享，风险明显更高。若退回 CPU 内存再上传 Vulkan，虽然可以快速验证协议，但会削弱 Pyrowave 的主要低延迟优势。

### 4.4 当前仓库中的 Vulkan 能力边界

当前代码定义了一个基于 FFmpeg Vulkan 的 `vulkan` 编码器，包含 `h264_vulkan`、`hevc_vulkan` 和 `av1_vulkan`，输入格式为 NV12/P010，编码器定义没有提供 YUV 4:4:4 输入：[video.cpp](../src/video.cpp:1649)。这表示仓库中存在 Vulkan 编码器描述和 FFmpeg Vulkan 设备初始化代码，不代表现有捕获纹理已经能够直接进入 Vulkan 编码器。

当前 Windows 显示工厂主要根据 `dxgi` 和 `system` 创建 DDX/WGC/VDD 捕获对象：[display_base.cpp](../src/platform/windows/display_base.cpp:1262)。Linux 的 Wayland、KMS 捕获路径主要处理 `system`、`vaapi` 和 `cuda`，没有看到与 `mem_type_e::vulkan` 对应的完整捕获对象接入：[wlgrab.cpp](../src/platform/linux/wlgrab.cpp:381) [kmsgrab.cpp](../src/platform/linux/kmsgrab.cpp:1534)。因此现有 Vulkan encoder 目前应视为已定义但链路尚未完整接通的能力，是否能在特定构建和 FFmpeg 环境下运行仍需单独验证。

Windows 中的 Vulkan HDR Bridge 是另一条独立能力。它为 ZakoVDD 的 Vulkan 应用临时注册 HDR 兼容层，作用是改善 Vulkan 应用向 VDD HDR 输出的呈现，不负责屏幕捕获、Vulkan 编码或 D3D11 到 Vulkan 的资源导入：[vulkan_hdr_bridge.md](vulkan_hdr_bridge.md:1)。

因此，Pyrowave 所需的能力仍然缺失以下关键环节：

```text
D3D11 捕获纹理
    → Vulkan 外部内存导入
    → D3D11/Vulkan 外部同步
    → Pyrowave Vulkan Compute 编码
```

这也是 Pyrowave 需要独立编码后端和资源互操作层的原因，不能直接复用现有 NVENC、AMF 或当前 FFmpeg Vulkan 编码器定义。

## 5. 网络和客户端影响

Pyrowave 的带宽成本是主要产品风险。PyroFling 项目给出的 1080p60 示例使用 250 Mbps，且额外的 FEC 会继续增加带宽；4K、120 FPS 或高质量 4:4:4 场景可能明显高于这个数值。

参考：[Themaister/pyrofling](https://github.com/Themaister/pyrofling)

影响范围包括：

- 普通 Wi-Fi、百兆网络和存在带宽限制的远程网络不适合默认启用；
- 路由器队列、MTU、UDP 丢包和 FEC 开销需要重新评估；
- 客户端需要真正支持 Vulkan Compute 或等效的 GPU 解码路径；
- Moonlight PC、Android、iOS、Switch 以及 Foundation 生态中的定制客户端都需要分别适配；
- 旧客户端必须继续使用已有视频格式，不能因为服务器支持 Pyrowave 就改变默认协商结果。

PyroFling 已经有自己的 `pyro://` 传输、客户端、服务端和 Android 实验代码，但它是独立协议，不能直接替代 Moonlight 的 RTSP/RTP 链路。它更适合作为性能和算法参考实现。

## 6. 主要风险

| 风险 | 影响 | 处理建议 |
|---|---|---|
| 协议不兼容 | 现有 Moonlight 客户端无法解码 | 先完成能力协商和客户端 PoC，服务端保持回退 |
| 高带宽 | Wi-Fi、百兆网或 WAN 下卡顿、丢帧 | 仅手动开启，限制在有线千兆环境 |
| Vulkan 设备不匹配 | 混合显卡、VDD 或多 GPU 下导入失败 | 按适配器 LUID 选择设备，失败后回退 |
| D3D11/Vulkan 同步错误 | 花屏、撕裂、GPU hang | 单独封装外部内存和同步生命周期 |
| API/ABI 未稳定 | 上游升级造成构建或运行时破坏 | 暂不依赖动态 ABI，固定版本并隔离模块 |
| 驱动覆盖不足 | 部分旧 GPU 无法运行 Vulkan 1.3 特性 | 启动时能力探测，保留 H.264/HEVC/AV1 |
| 资源生命周期错误 | VDD、串流恢复或断开时崩溃 | 将编码资源绑定到 RTSP 会话，完整覆盖取消和断开路径 |

Pyrowave 项目采用 MIT 许可证，但引入其构建依赖、Vulkan 运行库和客户端组件时仍需单独核对发行包和第三方许可证边界。

## 7. 推荐的 PoC 方案

建议第一阶段只选择 Windows 服务端和一个可控客户端，例如 Moonlight V+，暂时不修改所有客户端。

### 服务端

1. 固定一个 Pyrowave 版本，先静态集成，不依赖不稳定的动态 ABI。
2. 新增独立的 `pyrowave` 编码模块，不改动已有 H.264、HEVC、AV1 实现。
3. 从当前 D3D11 捕获纹理导入 Vulkan，验证同一适配器下的零拷贝路径。
4. 初期只支持 SDR、1080p60、4:2:0 和有线网络。
5. 导入或同步失败时立即回退到现有编码器。

### 客户端

1. 增加实验性视频格式能力声明。
2. 增加 Pyrowave 帧分片和重组。
3. 使用 Vulkan Compute 解码并输出到渲染纹理。
4. 加入解码耗时、丢包、重组超时和 GPU 内存统计。

### 验收指标

- 1080p60 下编码和解码路径稳定运行 30 分钟以上；
- 不经过 CPU 帧拷贝；
- 在 1 Gbps 有线网络中对比 AV1/HEVC 的编码、网络、解码和显示延迟；
- 模拟丢包后能在下一帧恢复；
- 断开、Resume、VDD 销毁和客户端切换不会泄漏 Vulkan/D3D11 资源；
- 不支持 Pyrowave 的客户端仍能正常使用现有编码格式。

## 8. 最终建议

当前最合理的处理方式是将 Pyrowave 作为实验性研究需求，不立即进入主线实现。先完成“单 GPU、D3D11 到 Vulkan 零拷贝、单客户端、1080p60”的 PoC，再根据实际端到端延迟决定是否扩展到 HDR、4:4:4、4K 和其他客户端。

Pyrowave 的价值主要在局域网极低延迟场景。它不能替代现有 H.264、HEVC 和 AV1，也不适合作为默认编码格式。
