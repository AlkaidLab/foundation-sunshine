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
与 PyroWave bitstream 一起参与 block-aware FEC。PyroWave 尚未发布旧 wire，因此不实现旧格式兼容或
迁移分支；没有声明该能力的客户端继续使用传统编码格式。

客户端与服务端必须同时支持：

- API 和 bitstream 版本；
- 视频尺寸、帧率和最大包长度；
- SDR、HDR10/PQ、HLG 及对应的 limited/full 色彩范围；
- 帧分片、重组、超时和丢包恢复规则。

主机处理耗时通过可选 Runtime TLV 传递；丢失时只影响诊断，不影响视频。静态 HDR 呈现信息
仍通过现有控制通道传递，不能把 `SS_HDR_METADATA` 混入 PyroWave color metadata。

旧版 Moonlight 不声明 PyroWave 能力，因此继续使用 H.264、HEVC 或 AV1。实验客户端
显式选择 PyroWave 但不满足协议或设备条件时，应在媒体开始前拒绝本次连接并记录原因，
不在同一次连接中自动切换传统编码格式。

## HDR 处理边界

PyroWave 的码流颜色信息与 Sunshine 的静态 HDR 呈现信息不是同一类数据：

- 码流合同描述 primaries、transfer、YCbCr 变换、range 和 chroma siting；
- Sunshine 的静态 HDR 信息通过现有控制通道传递到客户端呈现层；
- HDR10/PQ 在客户端具备对应呈现能力时应用静态 HDR 信息；
- HLG 没有完整 mastering metadata 时仍保持 HLG 呈现，不伪造 HDR10 metadata；
- `maxFullFrameLuminance` 会随 `SS_HDR_METADATA` 传递并校验，但 Vulkan 的 `VkHdrMetadataEXT`
  没有独立字段，因此当前呈现层只保留该值，不把它错误映射为 MaxFALL；
- 动态 HDR10+ 的 TLV 类型已经预留，但当前不产生也不由 Vulkan 呈现；HDR10+、HDR Vivid 和
  Dolby Vision 仍不属于当前可用能力。

缺少必要能力时，媒体开始前拒绝本次 PyroWave 连接。静态 HDR 元数据缺失或无法校验时，
不伪造默认值，保留已协商的 PQ/HLG 色彩空间并明确记录降级状态；运行中的 Vulkan/Surface
不可恢复错误仍只结束当前视频会话。

## 失败处理

PyroWave 的资源、设备和协议错误必须限定在当前视频会话：

1. 协商失败，或初始化失败且没有支持的恢复路径：结束当前 PyroWave 连接，不静默改用传统编码格式；
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

PyroWave API/ABI 仍处于 0.x，项目必须固定版本并在升级时重新验证 Sunshine 与客户端的合同。

## 当前范围

当前研究只覆盖：

- Windows Sunshine；
- 配套 Moonlight V+ Android 实验版本；
- SDR、静态 HDR10/PQ、静态 HLG；
- 4:2:0、limited/full range；
- SDR 8-bit、HDR10/PQ 与 HLG 10-bit；不提供独立的 SDR 10-bit 模式；
- 高带宽局域网；
- 未选择 PyroWave 时，传统视频格式仍按既有规则协商；这不表示显式 PyroWave 连接会自动改用其他编码器。

PyroWave 的位深与传输模式固定为：

| `dynamicRange` | 信号 | 位深 | 范围 |
|---:|---|---:|---|
| `0` | SDR / BT.709 | 8-bit | limited 或 full |
| `1` | HDR10 / PQ / BT.2020 | 10-bit | limited 或 full |
| `2` | HLG / BT.2020 | 10-bit | limited 或 full |

协议没有独立的 SDR 10-bit 选项；编码器探针显示的 SDR 10-bit 能力不代表该模式可由
Moonlight 选择。

Linux、其他客户端、动态 HDR、4K、4:4:4、高帧率全覆盖以及正式发行承诺，均不在当前范围内。

## 参考资料

- [PyroWave](https://github.com/Themaister/pyrowave)
- [PyroWave C API](https://raw.githubusercontent.com/Themaister/pyrowave/master/pyrowave.h)
- [PyroWave bitstream draft](https://github.com/Themaister/pyrowave/blob/master/bitstream/bitstream.md)
- [PyroFling](https://github.com/Themaister/pyrofling)
- [Steam PyroWave beta announcement](https://steamcommunity.com/groups/homestream/discussions/0/564794422009744473)
- [Vulkan external memory and synchronization](https://docs.vulkan.org/guide/latest/extensions/external.html)
