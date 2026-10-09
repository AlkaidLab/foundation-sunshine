# PyroWave 与 Foundation Sunshine 集成研究

## 结论

PyroWave 适合在高带宽局域网中作为实验性视频格式进行研究。它可能降低编码和解码等待时间，但通常需要更多网络带宽，因此不能替代 H.264、HEVC 和 AV1，也不适合作为默认编码格式。

当前建议只面向 Windows Sunshine 和配套的 Moonlight V+ Android 实验版本，保持旧客户端和旧编码格式兼容。

## 端到端架构

```text
Windows 捕获
    → Sunshine 现有画面处理
    → PyroWave 视频编码
    → Sunshine 视频传输
    → Moonlight V+ 协议重组
    → PyroWave GPU 解码
    → 客户端 Surface 呈现
```

PyroWave 作为独立的视频编码后端接入共享视频管线，不复制一套新的捕获或会话循环。音频、麦克风、输入、USB、控制器和剪贴板通道继续使用现有实现。

Windows 构建直接使用仓库中的 PyroWave CMake 子项目，将 C API、核心和必要依赖静态链接进 Sunshine。PyroWave、Granite 和嵌套依赖的版本由各自的子模块 gitlink 固定，不在构建脚本中重复维护 SHA，也不复制一份源码参与构建。运行时不依赖单独的 PyroWave DLL。

## 协议与兼容性

PyroWave 现在使用独立的 `PYRF` Frame Envelope。当前合同字段固定为
`protocolVersion=2`、`bitstreamVersion=2`、`payloadVersion=3`；每个内层包使用固定
Frame Header，并区分 Frame Header、data 和 parity 包。metadata 使用 TLV，受保护 metadata
与 PyroWave bitstream 一起参与 block-aware FEC。两端共享这一份合同，不提供早期实验包格式的
解析分支；没有声明该能力的客户端继续使用传统编码格式。

客户端与服务端必须同时支持：

- API 和 bitstream 版本；
- 视频尺寸、帧率和最大包长度；
- SDR、HDR10/PQ、HLG 及对应的 limited/full 色彩范围；
- 帧分片、重组、超时和丢包恢复规则。

主机处理耗时通过可选 Runtime TLV 传递；丢失时只影响诊断，不影响视频。静态 HDR 呈现信息
仍通过现有控制通道传递，不能把 `SS_HDR_METADATA` 混入 PyroWave color metadata。

旧版 Moonlight 不声明 PyroWave 能力，因此继续使用 H.264、HEVC 或 AV1。实验客户端
初始能力协商未选中 PyroWave 时，可沿用 common-c 的 H.264 兼容兜底，并提示实际格式。
PyroWave 预检失败、ANNOUNCE 拒绝及运行中故障不因此新增自动 codec 重试。

## HDR 处理边界

PyroWave 的码流颜色信息与 Sunshine 的静态 HDR 呈现信息不是同一类数据：

- 码流合同描述 primaries、transfer、YCbCr 变换、range 和 chroma siting；
- Sunshine 的静态 HDR 信息通过现有控制通道传递到客户端呈现层；
- HDR10/PQ 在客户端具备对应呈现能力时应用静态 HDR 信息；
- HLG 没有完整 mastering metadata 时仍保持 HLG 呈现，不伪造 HDR10 metadata；
- `maxFullFrameLuminance` 会随 `SS_HDR_METADATA` 传递并校验，但 Vulkan 的 `VkHdrMetadataEXT`
  没有独立字段，因此当前呈现层只保留该值，不把它错误映射为 MaxFALL；
- HDR10+、Vivid PQ/HLG、DV 8.1/8.4 的桌面生成子集独立协商，并通过同帧受保护 TLV
  交给客户端应用内亮度映射，输出仍为 PQ/HLG；不是厂商原生 Dolby Vision 显示模式。

动态元数据与图像由同一帧分配拥有，参与相同的 FEC 和 deadline。主机逐帧分析编码画面，
在 GPU Fence 完成后取得当前统计；客户端只消费随当前 decode unit 到达的完整 payload，
不能在丢帧、重建或 Surface 变化后套用另一帧的动态状态。生成范围不含影片 authored curves、
Dolby trim/增强层或任意 Profile。

缺少必要能力时，媒体开始前拒绝本次 PyroWave 连接。静态 HDR 元数据缺失或无法校验时，
不伪造默认值，保留已协商的 PQ/HLG 色彩空间并明确记录降级状态；运行中的 Vulkan/Surface
不可恢复错误仍只结束当前视频会话。

## 失败处理

PyroWave 的资源、设备和协议错误必须限定在当前视频会话：

1. 已选中 PyroWave 后的协商失败，或初始化失败且没有支持的恢复路径：结束当前连接，不静默改用传统编码格式；初始能力协商的 H.264 兼容兜底与此分开；
2. 没有 GPU 专属增强且独占共享捕获的 SDR 会话允许重建为 CPU PyroWave；存在其他会话时不改变共享捕获类型，只结束失败会话。CPU 捕获期间拒绝不兼容的新 GPU 会话，显示重建继续使用 CPU 图像；HDR、增强路径和 CPU 恢复失败时结束当前视频会话；
3. 客户端解码或呈现发生不可恢复错误时结束当前视频会话，不在运行中切换传统编码器；
4. 会话结束后释放本次视频资源；
5. 不影响 Sunshine 进程、音频通道和其他客户端。

## 主要风险

### 带宽

PyroWave 的带宽需求可能明显高于 HEVC 和 AV1。普通 Wi-Fi、百兆网络和远程网络不应默认使用实验格式。

### 硬件差异

GPU、驱动、显示设备、虚拟显示器和混合显卡会影响 GPU 编解码与显示呈现。单一设备上的成功不能推导出所有硬件都兼容。

### 生命周期

显示切换、设备丢失、恢复连接和长时间运行需要单独验证，不能仅凭一次启动成功判断资源生命周期完整。

### 上游变化

上游 PyroWave 已冻结 1.0.0 C API/ABI。本项目使用保留颜色/HLG 扩展的 101.0.0，
通过子模块固定源码版本；升级时仍需分别核对原生 ABI、码流扩展和网络传输合同。
上游的 `PWV1Header` 是文件/容器头，不替代 common-c Frame Envelope。

## 当前范围

当前研究只覆盖：

- Windows Sunshine；
- 配套 Moonlight V+ Android 实验版本；
- SDR、HDR10/PQ、HLG，以及桌面生成 HDR10+/Vivid/DV 子集的应用内动态映射；
- 4:2:0、limited/full range；
- SDR 8-bit、HDR10/PQ 与 HLG 10-bit；不提供独立的 SDR 10-bit 模式；
- 高带宽局域网；
- 未选择 PyroWave 时，传统视频格式仍按既有规则协商。显式选择 PyroWave 的初始能力协商可沿用 H.264 兼容兜底，客户端明确提示实际格式；预检失败、ANNOUNCE 拒绝和运行中故障不因此新增自动 codec 重试。

PyroWave 的位深与传输模式固定为：

| `dynamicRange` | 信号 | 位深 | 范围 |
|---:|---|---:|---|
| `0` | SDR / BT.709 | 8-bit | limited 或 full |
| `1` | HDR10 / PQ / BT.2020 | 10-bit | limited 或 full |
| `2` | HLG / BT.2020 | 10-bit | limited 或 full |

协议没有独立的 SDR 10-bit 选项；编码器探针显示的 SDR 10-bit 能力不代表该模式可由
Moonlight 选择。

Linux、其他客户端、厂商原生动态 HDR、4K/高帧率全覆盖、4:4:4 以及正式发行承诺，均不在当前范围内。

## 参考资料

- [PyroWave](https://github.com/Themaister/pyrowave)
- [PyroWave C API](https://raw.githubusercontent.com/Themaister/pyrowave/master/pyrowave.h)
- [PyroWave bitstream draft](https://github.com/Themaister/pyrowave/blob/master/bitstream/bitstream.md)
- [PyroFling](https://github.com/Themaister/pyrofling)
- [Steam PyroWave beta announcement](https://steamcommunity.com/groups/homestream/discussions/0/564794422009744473)
- [Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html)
