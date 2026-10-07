# PyroWave 集成边界

本目录保存 Sunshine 侧的 PyroWave 会话、能力和 GPU 互操作边界。Windows 构建默认
静态编译 PyroWave C API、核心和 Granite 依赖；运行时只有在 Vulkan/GPU 互操作和
客户端能力均满足时才会广告和选择该格式。传统 NVENC、AMF、软件编码以及
H.264/HEVC/AV1 协商路径继续保留。

## Sunshine 侧的职责

- `types.*`、`capabilities.*`、`session.*`：保存协议版本、能力交集和每个串流
  会话的生命周期状态。
- `gpu_interop.*`、`packet.*`、`packetizer.*`：定义 GPU 图像、同步对象、编码帧
  和 Sunshine RTP/队列之间的边界。
- `runtime.*`：执行静态编译实现的 API 版本和基础 Vulkan 互操作探针。探针只是
  一个进程级的早期门禁，不代表当前捕获显示器一定能建立完整会话。
- `src/platform/windows/pyrowave/*`：把 Windows 捕获结果交给 PyroWave。生产路径
  复用 Sunshine 已有的捕获、旋转、缩放、光标和 HDR/色彩处理；随后通过 D3D11
  共享资源、平面拆分和 Vulkan 外部同步交给 PyroWave 编码器。

## 当前视频数据路径

Windows 构建包含 PyroWave 后，典型的 GPU 流程是：

```text
D3D11 capture
  -> Sunshine rotation/scaling/cursor/HDR processing
  -> NV12/P010 conversion and R8/R16 Y/U/V plane split
  -> PyroWave Vulkan encode/packetize
  -> PyroWave block-aware FEC (one XOR parity block per up to 16 data blocks)
  -> Sunshine RTP payloads aligned to one PyroWave wire packet (without the legacy short frame header)
  -> Moonlight PyroWave direct depacketizer/reassembly (legacy RTP parity disabled)
```

GPU 路径使用共享纹理和外部同步，不把每一帧读回 CPU。若 SDR 会话的 GPU
互操作或分裂着色器不可用，且没有启用 GPU 专属增强时，当前实现允许使用受限的
CPU staging 回退。切换共享捕获内存类型只允许在当前会话独占捕获时进行；有其他会话时只结束
失败的 PyroWave 会话。已进入 CPU 捕获后保留该类型直到相关会话结束，后续显示重建不恢复为
GPU 图像；不兼容的新 GPU 会话在接入时被拒绝，避免读取错误类型的图像。捕获后端覆盖只作用于
本次创建，不改写会话保存的捕获设置。HDR10/PQ、HLG 和 GPU 专属增强初始化失败时结束当前视频会话，
不尝试 CPU 回退。实际资源检查发生在编码器工厂中，不代表 ANNOUNCE 已完成全部 GPU 预检。显式选择
PyroWave 时不会在同一次连接中自动切换到 HEVC/AV1；用户需要修复能力或手动选择
其他编码器后重新连接。
设备兼容性由实际资源检查决定，不能从进程级探针推导所有显示器和 GPU 都可用。

PyroWave 的 HDR 合同目前覆盖 HDR10/PQ 和 HLG，并通过公共能力字段和 SDP 属性
传递色彩元数据。动态 HDR10+、Dolby Vision、HDR Vivid 等格式不在当前合同中，
不能因为系统或客户端报告了 HDR 就假定它们已经可用。

像素转换、平面位深和码流颜色元数据都以实际解析的输出色彩空间为准。若请求 HDR 而
捕获结果仍为 SDR，且没有产生 HDR 的增强管线，则拒绝创建该 PyroWave 会话；不能把 SDR
像素标记为 PQ/HLG，也不静默改为未协商的 SDR 10-bit。

编码器仍严格遵守每帧码率上限。针对低 bits-per-pixel 的串流，RDO 会在不增加帧预算的
前提下提高亮度高频小波带的权重，把有限码字优先留给边缘和纹理；这不会切换到其他
编解码器，也不会修改客户端选择的目标码率。帧预算固定按协商帧率计算，支持整数和分数
帧率；初始预算和动态码率更新使用同一规则，不根据单帧编码间隔逐帧改变硬上限。
捕获等待超时和最低刷新帧率只影响静态画面等待行为，不是编码频率上限，不能用来放大
单帧预算。本预算约束编码数据，不代表包含 FEC、包头和音频的总网络流量上限。

当前 PyroWave Frame Envelope 使用 `PYRF` 64 字节固定头，区分 Frame Header、data 和 parity
packet，并携带 codec/protected payload 长度、metadata TLV 长度、data/parity 数量、group、
shard 和 block 容量。metadata 与编码数据组成受保护 payload；每个线上 packet 都填充到
block 容量，保证它与一个外层 RTP payload 对齐。客户端先恢复一个 group 内最多一个丢失
data block，先提取 metadata，再依据 codec payload 长度去掉 metadata 和末尾填充，将纯编码
数据交给 decoder。恢复不了的帧在 deadline 后丢弃。
块级 FEC 是协商的必需能力；恢复规则与帧时限由公共协议实现统一校验。
Frame Header 还携带完整 metadata 副本，因此 metadata 必须能放入扣除 64 字节封套头后的
单包容量。使用默认包长时遵守相同限制；超限请求在复制前拒绝，不截断 TLV。

主机处理耗时使用受保护的可选 Runtime TLV。只有 metadata 非空时才设置外层 metadata
标记；重复帧不携带该 TLV。发布层区分分包成功与合法的失败结果，失败不入队，也不发送空帧。

Sunshine 的 `SS_HDR_METADATA` 是另一条现有控制通道，包含 RGB primaries、white
point、mastering luminance、MaxCLL、MaxFALL 和显示器 full-frame luminance。Moonlight
V+ 会把它作为独立的呈现元数据传给 Vulkan decoder；它不写入 PyroWave
`pyrowave_color_metadata`。HDR10/PQ 在 metadata 已提供且有效时通过
`VK_EXT_hdr_metadata` 应用静态呈现信息；控制通道尚未提供 metadata 时仍保持 HDR10/PQ
色彩空间，但不伪造默认值，并记录为降级 HDR。已提供但非法的 metadata 不应用、不伪造
替代值，保留已协商的 HDR 色彩空间并标记为降级 HDR；HDR10/PQ 还要求 Vulkan 扩展可用
才能应用完整静态 metadata。HLG 可以在没有完整
mastering metadata 时保持 HLG 色彩空间，但不会伪造 HDR10 metadata；如果 HLG metadata
已经提供而 Vulkan 扩展不可用，则继续使用 HLG 色彩空间并记录缺少静态 metadata 的降级状态。
Vulkan 的 `VkHdrMetadataEXT` 没有独立的 `maxFullFrameLuminance` 字段；该值会在验证后
保留于客户端快照并随 swapchain 重建保存，但不会被误映射为 MaxFALL。

## 探针与失败边界

`is_server_runtime_available()` 缓存默认 Vulkan 设备、API 版本和基础互操作结果，
用于避免在 SDP 生成阶段反复初始化运行库。它不会验证：

- 当前捕获显示器对应的 GPU LUID；
- 当前 D3D11 共享纹理、三个 Y/U/V 平面和 fence；
- 当前编码尺寸、信号格式和分裂 shader。

因此，实际会话仍必须在创建编码器时做资源检查。资源创建失败必须局限在当前
PyroWave 会话：SDR 只允许明确验证过的 CPU staging 路径；HDR/HLG 直接结束本次
PyroWave 会话，不能让 Sunshine 进程退出，也不在当前连接中静默切换传统编码器。客户端在 RTSP 启动前还会进行
Surface、尺寸和实际 Vulkan 创建预检，避免仅凭默认设备探针广告一个当前不可用的
会话。

### 诊断与性能记录

探针、D3D11/Vulkan 资源导入、fence、颜色合同、编码、分包和客户端 Surface/交换链失败
都会在本地日志中记录阶段、API/Vulkan 返回码以及必要的帧号、尺寸、码流大小或块数。
成功帧不逐帧写诊断日志；客户端对同一失败阶段只记录一次，避免网络或驱动持续失败时刷屏。

`/api/diagnostics/performance`（以及现有性能快照调用方）中的会话对象增加 `pyrowave`
计数器：编码帧数、编码失败、分包失败和恢复失败。会话停止时不立即删除统计，在线程
真正退出的 `join()` 阶段收尾，并有界保留最近八个已结束会话，便于排查断流末尾的失败。
这些字段只用于本机排障，不属于 Moonlight 线协议。

## API 与可复现构建

当前 Sunshine 适配器使用 PyroWave API `0.6.1`、protocol version `2`、bitstream version `2`
和 payload version `3`。源版本由 `third-party/pyrowave` 的子模块 gitlink 固定，Granite、
Volk 和 Vulkan-Headers 由其嵌套子模块固定。Sunshine 使用 `add_subdirectory()` 直接构建
`pyrowave-c-api-static`，通过 CMake target 链接 C API、核心和必要的 Granite 静态依赖。
不复制源码、不应用本地补丁，也不在构建脚本中维护另一份提交 SHA。源文件修改由
CMake/Ninja 的依赖图跟踪，所有产物位于构建目录，不需要单独运行 PowerShell 脚本。

common-c 协议、PyroWave 子模块和分裂着色器均由版本控制固定；构建基线不依赖本地补丁
或手工生成的运行库副本。

## 验证范围

- `pyrowave_unit_tests`：CPU-only 合同、能力和生命周期校验。
- 静态链接验证：主程序、单元测试和安装暂存树不依赖额外 PyroWave DLL。
- 真实 GPU、跨 API 资源导入、HDR、旋转、设备丢失和端到端串流验证不属于普通
  Sunshine 构建步骤，需要在专用测试环境中执行。

## 许可证与依赖

PyroWave、Granite、Volk 和 Vulkan-Headers 的许可证通知随安装/发布流程保留。Granite
由 PyroWave 的嵌套子模块提供，不成为 Sunshine 的独立运行时依赖；PyroWave C API、
核心和 Granite 静态归档会在链接阶段合并进 Sunshine，不需要额外的
`libpyrowave-shared-0.dll`。
