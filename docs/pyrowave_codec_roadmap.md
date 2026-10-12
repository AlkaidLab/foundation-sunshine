# PyroWave Codec 路线图

## 目标

PyroWave 是面向高带宽局域网的实验性视频格式。它采用帧内编码和 GPU 计算，目标是降低编码与解码等待时间，但带宽需求通常高于 H.264、HEVC 和 AV1。

现有 H.264、HEVC 和 AV1 仍是默认视频格式。PyroWave 只有在客户端和服务端都明确支持并完成协商时才会使用。

## 当前支持范围

| 项目 | 范围 |
|---|---|
| 服务端 | Windows Sunshine 实验版本 |
| 客户端 | 配套的 Moonlight V+ Android 实验版本 |
| 视频 | SDR、HDR10/PQ、HLG；Vivid 使用原生 Vulkan 能力，HDR10+ 与 Dolby Vision 使用客户端应用映射 |
| 色彩 | BT.709 或 BT.2020、4:2:0、limited/full range |
| 位深 | SDR 固定 8-bit；HDR10/PQ 与 HLG 固定 10-bit |
| 网络 | 高带宽局域网，优先使用有线网络 |
| 默认行为 | 不改变现有编码器的默认选择 |

Windows Sunshine 通过 CMake 子项目将 PyroWave 核心和 C API 静态链接进主程序，不需要单独的构建脚本或额外安装 PyroWave DLL。源版本由子模块 gitlink 固定。客户端是否支持 PyroWave 由配套版本和运行能力决定。

原生 API 使用基于上游 1.0.0 的 101.0.0，保留颜色读取/设置和 HLG 扩展。
Sunshine 保持默认 context 0 的同步交付，不因上游提供异步批处理接口而改变串流节奏。
原生 ABI、文件封装和网络 Frame Envelope 分别管理，不能以某一版本号代替其他合同的校验。

颜色范围只影响 YUV 信号的量化范围，不改变上述位深合同。PyroWave GPU 编解码路径会在
对应的 SDR 8-bit 或 HDR 10-bit shader 中完成 limited/full 归一化。

颜色标签必须与实际像素转换和位深一致。请求 HDR 但显示器及增强管线都没有提供 HDR
输出时，不把 SDR 数据伪装成 PQ/HLG。GPU 失败只能在独占共享捕获的 SDR 会话中切换为
CPU 捕获；不得为单个 PyroWave 会话改变其他串流的图像内存类型。

帧预算固定按协商帧率计算，不按捕获等待超时、最低刷新帧率或单帧实测间隔放大预算，
避免内容更新频率升高时码率超出预算，也避免逐帧预算变化导致静态文字质量闪动。
FEC、包头和音频仍有额外网络开销。低 bits-per-pixel 时仍会优先保留亮度高频细节，但 PyroWave
是帧内编码，低码率画质不能直接与 HEVC/AV1 的帧间编码效果等同比较。

## 设计原则

### 当前 Frame Envelope 合同

PyroWave 传输合同直接冻结为 `PYRF` Frame Envelope（当前字段值为
`protocolVersion=2`、`bitstreamVersion=2`、`payloadVersion=3`）：

- 每个内层包使用固定 Frame Header，并区分 `FRAME_HEADER`、`DATA`、`PARITY`；
- 内层完整包长受协商后的 `maxPacketSize` 约束，外层 RTP 同步缩小分块容量，保持一包一块；
- metadata 使用 TLV，受保护 metadata 与 PyroWave bitstream 一起参与 block-aware FEC；
- 主机耗时通过可选 Runtime TLV 传递，丢失时不影响视频；
- `SS_HDR_METADATA` 仍走现有控制通道，不复制到 PyroWave color metadata。

Sunshine 与配套 Moonlight V+ 共享这一份合同，不保留早期实验包格式的解析分支。
没有声明 PyroWave 能力的客户端仍按传统编码格式协商。

### 共享视频管线

PyroWave 只替换视频编码和解码后端，继续使用 Sunshine 现有的捕获、显示处理、会话管理、停止流程和错误边界。音频、麦克风、输入、USB、控制器和剪贴板通道不改变。

### 明确协商

客户端和服务端需要同时确认固定合同字段、能力、颜色范围和 HDR 类型。limited/full range
都属于正式的 PyroWave 颜色合同，客户端的现有颜色范围设置会原样参与协商，不会被强制改成
full range。未选择 PyroWave 的客户端继续保留传统编码格式。初始能力协商未选中 PyroWave
时，可沿用 common-c 的 H.264 兼容兜底，并由客户端提示实际格式；预检失败、ANNOUNCE
拒绝或已建立会话中的故障不因此新增自动 codec 重试，也不静默切换到 HEVC/AV1。

### HDR 分层

码流颜色信息与显示呈现信息分开处理：

- SDR、HDR10/PQ 和 HLG 通过明确的颜色合同协商；
- Sunshine 的静态 HDR 呈现信息通过现有控制通道传递；
- HLG 不伪造 HDR10 mastering metadata；
- PyroWave 在原生 Vulkan Vivid 能力可用时按帧提交 Vivid PQ/HLG 动态 metadata；HDR10+ 与
  Dolby Vision 8.1/8.4 由客户端应用映射到各自协商的 PQ/HLG 基础信号。应用映射不代表
  Dolby 或 HDR10+ 的厂商认证输出。

### 动态 HDR 帧交付

动态会话在客户端现有 HDR 选项中选择；PyroWave 的动态选项包括原生 Vivid 以及
HDR10+/Dolby Vision 应用映射。主机需要开启 HDR 亮度分析，客户端在连接前验证对应
Vulkan Surface、静态 HDR metadata 和动态映射能力。双方确认动态格式及能力后才进入媒体阶段。

原生 Vivid、HDR10+ 和 Dolby Vision metadata 使用 Sunshine 现有的异步亮度分析和会话内场景快照；
客户端按协商格式分别执行原生 Vivid 提交或应用映射。
无新样本或新统计无效时保留有效状态，不因此丢帧；编码器重建时清空场景历史。

完整动态 HDR payload 与该帧码流一起参与 FEC，重组后随 decode unit 传到 Vulkan 原生或应用映射阶段。
主机复用场景统计不等于省略逐帧 metadata：接收端仍拒绝缺失或非法的必需 TLV，不能用
客户端上一帧的 metadata 补齐。没有有效状态时不生成默认动态 metadata。

PyroWave 原生 Vivid 只接受 Sunshine 生成的四项统计和 UWA base curve 子集，
Vivid/PQ 使用 `system_start_code=0x01`，Vivid/HLG 使用 `0x02`。metadata 正文在实际
Vulkan device 启用 Vivid feature 后通过原生接口提交，不在客户端生成 Vivid 亮度 LUT。
HDR10+ 的 ST 2094-40 单窗口曲线和 Dolby Vision 8.1/8.4 的受支持 RPU 字段由客户端应用映射；
Dolby Profile 5/7、NLQ、增强层和影片 authored metadata 仍由传统 HEVC 路径负责。

### 失败隔离

- 媒体开始前发现能力或初始化不满足时，拒绝本次 PyroWave 连接并记录原因；
- 无 GPU 专属增强的 SDR 会话在 GPU 后端故障时可通过共享捕获重建切换到 CPU PyroWave；
- HDR、GPU 专属增强或 CPU 恢复失败时结束当前视频会话，不在连接中改用传统编码器；
- 不因 PyroWave 故障退出 Sunshine，也不影响其他媒体通道；
- 不支持 PyroWave 的旧客户端继续使用现有视频格式。

## 尚未承诺的能力

- 默认启用 PyroWave；
- Linux 服务端和其他客户端平台；
- 厂商原生 HDR10+/Dolby 输出、原生 Vivid 的设备全覆盖、任意影片动态曲线、Dolby Profile 5/7 和增强层；
- 4K、4:4:4、4:2:2 和高帧率全覆盖；
- 混合 GPU、多 GPU、多客户端和所有 VDD 场景；
- 固定带宽、固定丢包率或固定延迟保证；
- 稳定的公开 PyroWave API/bitstream 兼容承诺。

## 扩展规则

- 平台、像素格式或 HDR 类型的扩展必须同步维护服务端、common-c 和客户端的能力合同。
- 依赖升级必须固定子模块提交，并验证运行库与接口版本一致。
- 显示切换、设备丢失、恢复连接和长期运行属于独立的生命周期验证场景。
- 延迟、带宽、画质和资源占用的对照必须使用相同的主机、客户端和网络条件。
- 增加能力不能改变旧客户端的默认格式选择，也不能突破既有错误隔离边界。

## 参考资料

- [PyroWave](https://github.com/Themaister/pyrowave)
- [PyroWave C API](https://raw.githubusercontent.com/Themaister/pyrowave/master/pyrowave.h)
- [PyroWave bitstream draft](https://github.com/Themaister/pyrowave/blob/master/bitstream/bitstream.md)
- [PyroFling](https://github.com/Themaister/pyrofling)
- [Steam PyroWave beta announcement](https://steamcommunity.com/groups/homestream/discussions/0/564794422009744473)
