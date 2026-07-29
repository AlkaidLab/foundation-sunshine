# Foundation Sunshine Windows D3D12 视频链路渐进式升级实施方案

- 状态：Draft
- 基线：`origin/master`，包含 HDR Vivid 分析修复 #869
- 适用范围：Windows 捕获、RGB 到 YUV 转换、HDR 动态元数据分析及硬件编码输入链路

## 1. 决策摘要

本项目不做一次性的“全量 D3D12 重写”。推荐采用可逐阶段交付、可逐会话回退的混合架构：

```text
WGC / Desktop Duplication / VDD
              |
        D3D11 capture texture
              |
      shared resource + shared fence
              |
       D3D12 compute queue
  RGB -> P010/NV12 + HDR reduction
       |                    |
       |                    +--> small readback -> HDR Vivid/HDR10+ metadata
       |
       +--> NVENC D3D12
       +--> AMF DX12
       +--> D3D11 bridge/fallback -> QSV/FFmpeg
```

优先顺序是：

1. 先补齐可观测性，取得可信基线；
2. 建立独立的 D3D12 计算后端，保持现有 D3D11 路径不变；
3. 等价移植 HDR 分析并验证结果一致性；
4. 将转换与分析融合，消除重复采样和不必要的全帧复制；
5. 先接 NVENC D3D12，再接 AMF DX12；
6. QSV/FFmpeg 是否迁移由实测收益决定，不作为首轮目标。

每个阶段都必须能单独合并、单独关闭。任一能力探测、资源共享、同步或设备恢复失败时，本次会话回退到现有 D3D11 路径，不影响串流可用性。

上图是逻辑目标，不预设 D3D11 捕获纹理能够被 D3D12 零拷贝打开。M1 必须先确定物理资源交接方式；如果全分辨率 RGBA bridge copy 的成本高于 D3D12 compute 收益，则保留 D3D11 转换，只把 D3D12 用于共享编码表面和原生编码器 fence，停止迁移 compute 热路径。

## 2. 背景与当前基线

当前 Windows 视频链路以 D3D11 为主：

- WGC、Desktop Duplication 和 VDD 提供 `ID3D11Texture2D`；
- `src/platform/windows/display_vram.cpp` 负责 GPU 内转换、HDR 分析和编码器表面交付；
- NVENC 使用 D3D11 native 或 D3D11-on-CUDA 路径；
- AMF 使用 `InitDX11()` 和 `CreateSurfaceFromDX11Native()`；
- FFmpeg/QSV 硬件帧以 `AV_PIX_FMT_D3D11` 为主要交接格式；
- Windows 构建当前链接 `d3d11`、`D3DCompiler` 和 `dxgi`，尚未引入 `d3d12` 或离线 DXIL 产物。

#869 合并后的 D3D11 快路径已经具备：

- RGB 到 P010/NV12 的 compute shader 转换；
- PQ、HLG、缩放和非缩放变体；
- 每 4 帧一次的 HDR 分析；
- 最大 1920x1080 的分析统计快照；
- 对每个整数分区完整扫描，保证非整数缩放比下的峰值不被点采样遗漏；
- 两阶段 min/max/average reduction 和 256 桶 PQ 直方图；
- GPU 异步 readback 未就绪时保留上一次有效元数据。

因此，D3D12 的价值不是“把像素着色器改成计算着色器”——这部分已经完成。主要机会在于：

- 使用显式队列和 fence 降低隐式同步；
- 用 Shader Model 6 wave reduction 减少组内 barrier 和共享内存压力；
- 在转换线程已经持有输出 RGB 样本时直接生成 group statistics，避免分析线程再次扫描同一像素；
- 让 NVENC/AMF 直接消费 D3D12 P010/NV12 表面，减少跨 API 复制和 ownership 切换；
- 将资源生命周期和背压变成可测、可控的固定 ring。

## 3. 目标与非目标

### 3.1 目标

- 保持 CUVA HDR Vivid、HDR10+ 和静态 HDR 元数据语义不变；
- 对编码后实际可见区域做分析，包含缩放后的像素结果；
- min/max 覆盖每个参与编码的像素，不因非整数缩放遗漏峰值；
- 稳态路径不出现 CPU 等待 GPU；
- Windows 10 支持不退化；
- NVIDIA、AMD、Intel 任一后端失败时能够自动回退；
- 设备丢失、分辨率切换、色彩空间切换和编码器重建可恢复；
- 通过数据证明收益后再自动启用。

### 3.2 非目标

- 不重写 WGC、Desktop Duplication 或 VDD 捕获接口；
- 不在首轮替换全部 D3D11 图形代码；
- 不改变 HDR Vivid/HDR10+ 的元数据生成公式、SEI/OBU 封装或发送时序；
- 不把 D3D12 Video Encode 作为基础编码后端；
- 不在验证完成前增加普通用户可见的复杂设置；
- 不承诺 QSV/FFmpeg 在首轮使用原生 D3D12 表面。

## 4. 设计原则

### 4.1 D3D11 捕获保留，D3D12 从计算阶段切入

捕获 API 的返回类型和稳定性决定了短期内 D3D11 仍是合理入口。D3D12 后端必须使用与 D3D11 设备相同的 adapter LUID，跨 API 共享仅发生在 Sunshine 自己创建和管理的资源上。

### 4.2 不在热路径使用 D3D11On12

D3D11On12 适合渐进迁移和简单互操作，但微软明确说明它未针对性能优化，可能带来中等 CPU 开销和显著内存开销。热路径采用原生 shared resource 与 shared fence；D3D11On12 只允许用于诊断原型，不进入发布实现。

### 4.3 正确性先于优化

M2 先实现与 D3D11 分析结果等价的 D3D12 版本。Wave 优化和转换融合在等价测试通过后启用，避免同时改变 API、同步模型和统计语义。

### 4.4 不阻塞即跳过，不追赶过期帧

HDR 动态元数据允许短时间复用上一个有效结果。分析 ring 无空闲 slot 或 readback fence 未完成时：

- 不等待；
- 不覆盖仍被 GPU 使用的资源；
- 保留上一份有效动态元数据；
- 记录 `analysis_skipped_busy`；
- 下一次正常分析周期再尝试。

视频帧本身不能因分析后端繁忙而丢失。

### 4.5 用能力探测和实测决定路径

启用条件不是“系统存在 D3D12”，而是以下条件全部满足：

- D3D12 device 可在捕获 adapter 上创建；
- Shader Model 6.0 和所需 wave operations 可用；
- D3D11/D3D12 shared resource 与 shared fence 自测成功；
- 对应编码后端支持目标表面格式；
- 启动时短基准或历史遥测没有显示显著回退。

### 4.6 先证明跨 API 交接不抵消收益

WGC 和 Desktop Duplication 返回的纹理不应假定带有可供 D3D12 打开的 NT shared handle；VDD 的 keyed-mutex 资源也不能假定能由 D3D12 直接遵循同一同步协议。M1 必须分别验证以下拓扑：

```text
A. 低复制优先
   D3D11 capture
     -> D3D11 compute
     -> D3D12-owned shared P010/NV12 opened by D3D11
     -> shared fence
     -> NVENC/AMF D3D12

B. 完整 D3D12 compute
   D3D11 capture
     -> copy to owned shared RGBA bridge
     -> shared fence
     -> D3D12 conversion/analysis
     -> NVENC/AMF D3D12
```

拓扑 A 是原生 D3D12 编码器接入的优先候选，因为它保留当前已经成熟的 D3D11 compute 转换，又有机会消除 P010 scratch-to-encoder copy。拓扑 B 只有在全分辨率输入 bridge 的净收益通过基准后才实施。

可额外做一个严格限时的 D3D11On12 原型来测量能否避免输入复制，但它不是默认设计；原型若没有在至少两家 GPU 上稳定胜出，应在 M1 结束时删除。

## 5. 目标模块边界

建议新增以下内部模块，避免继续扩大 `display_vram.cpp`：

```text
src/platform/windows/d3d12/
  d3d12_device.{h,cpp}          device、queue、fence、feature probing
  d3d12_shared_bridge.{h,cpp}   D3D11/D3D12 shared resource/fence
  d3d12_resource_ring.{h,cpp}   slot 生命周期和 fence values
  d3d12_video_compute.{h,cpp}   conversion/analysis PSO 与 dispatch
  d3d12_telemetry.{h,cpp}       timestamp queries 和结构化指标

src_assets/windows/assets/shaders/directx12/
  convert_nv12_p010_cs.hlsl
  hdr_reduce_cs.hlsl
```

首个实现只暴露一个窄接口：

```cpp
struct d3d12_video_result {
  ID3D12Resource *encoder_surface;
  ID3D12Fence *ready_fence;
  uint64_t ready_value;
  std::optional<hdr_luminance_stats> completed_hdr_stats;
};

class d3d12_video_compute {
 public:
  bool available() const;
  std::optional<d3d12_video_result> submit(const captured_frame &frame);
  void drain();
};
```

接口名称可以按现有代码风格调整，但不得把 D3D12 类型扩散到非 Windows 平台公共接口。M1/M2 阶段由 `display_vram.cpp` 持有可选后端；只有 M3 完成后，才评估是否值得抽象统一的转换后端。

## 6. 资源与同步模型

### 6.1 Slot ring

默认使用 3 个 slot。每个 slot 包含：

- D3D11 可写、D3D12 可读的共享输入资源；
- D3D12 P010 或 NV12 输出资源；
- group statistics、直方图及最终结果 buffer；
- 小型 readback buffer；
- command allocator；
- `capture_ready`、`compute_done`、`encode_done` 对应的 fence value；
- 当前宽高、格式、色彩空间和 generation。

所有 fence value 单调递增。slot 只有在 `encode_done` 和分析 readback ownership 都已释放后才能复用。

### 6.2 单帧时序

```text
CPU capture thread
  D3D11 writes/copies shared input[n]
  D3D11 Signal(capture_ready[n])
  submit D3D12 work and return

D3D12 compute queue
  Wait(capture_ready[n])
  input: COMMON -> NON_PIXEL_SHADER_RESOURCE
  output: COMMON -> UNORDERED_ACCESS
  dispatch convert (+ optional per-frame statistics)
  dispatch final reduction when analysis is due
  copy tiny result to readback
  output: UNORDERED_ACCESS -> COMMON
  Signal(compute_done[n])

Encoder
  Wait(compute_done[n])
  consume output[n]
  Signal(encode_done[n])

CPU metadata path
  poll completed analysis fence
  publish newest completed result, otherwise retain previous result
```

稳态禁止 `WaitForSingleObject()`、blocking `Map()` 或为同步目的调用 D3D11 `Flush()`。CPU 只允许轮询已完成 fence；关闭、重建和设备恢复属于例外。

### 6.3 Resource states

首版使用传统 D3D12 resource barriers，保持 Windows 10 兼容。Enhanced Barriers 仅在运行时能力探测成功后作为后续优化，不是 M1 的依赖。

跨队列或跨 API 交接时，共享资源回到 `COMMON`。每个 transition 的 before/after state 必须由 slot 状态机唯一维护，禁止调用方自行猜测状态。

### 6.4 设备丢失与重建

检测到 `DXGI_ERROR_DEVICE_REMOVED`、`DXGI_ERROR_DEVICE_RESET`、fence 异常或共享句柄失效时：

1. 记录 adapter LUID、removed reason、阶段和最后 fence values；
2. 停止接收新的 D3D12 work；
3. 尽力释放本后端资源，不阻塞正常关闭；
4. 本次串流会话禁用 D3D12；
5. 由现有 D3D11 路径继续，若底层 D3D11 设备也丢失则走现有重建流程。

分辨率、输出矩形、位深、codec 或 HDR/SDR 状态变化时，先停止复用旧 generation，drain 后重建 ring。旧 generation 的迟到结果必须丢弃。

## 7. Shader 实施

### 7.1 M2：分析等价移植

先把现有两阶段算法移植到 SM6：

- 优先输入 D3D11 转换器直接写入的共享 cell-statistics snapshot；
- D3D11 compute 快路径不可用时继续走现有 D3D11 full-frame fallback，不为 D3D12 analysis-only 新增 RGBA 全帧复制；
- 完整覆盖所有参与分析的整数像素；
- 每组计算 min、max、sum、count；
- 使用 256 桶归一化 PQ 直方图；
- 第二阶段归约为一个 `FinalResult`；
- CPU 继续按现有规则计算 P10/P90/P95/P99；
- readback 未完成时保持上一结果。

初版可保留当前 group-shared reduction，以便建立一一对应的 golden test。之后用 `WaveActiveMin`、`WaveActiveMax` 和 `WaveActiveSum` 替换 wave 内 reduction，再由每个 wave 的 lane 0 写入 group shared memory，最后归并 wave results。

### 7.2 M3：转换与分析真正融合

当前 D3D11 快路径在分析帧会生成 1920x1080 cell-statistics snapshot；每个选中线程重新扫描所属 cell。D3D12 目标实现不再生成这张全尺寸统计纹理：

1. 每个转换线程计算一次最终输出 RGB 样本；
2. 同一个样本用于写 Y，参与 UV 计算，并参与 HDR statistics；
3. wave/group reduction 直接生成该 16x16 输出 tile 的 min/max/sum/count；
4. group-shared 局部直方图只把非零桶 flush 到全局；
5. 第二阶段只归并每个 tile 的小型结果。

这样分析对象与编码输出一致，缩放场景分析 Catmull-Rom 后的输出样本；非缩放场景分析每个实际输出像素。它既保持真正的帧 extrema，又避免为分析重复执行纹理读取和缩放滤波。

本阶段受 M1 的复制成本门控。如果 D3D12 读取捕获结果必须先增加 RGBA16F 全帧复制，且该复制未被消除的 P010 copy、wave reduction 和编码器交接收益覆盖，则不实施本阶段。此时可把“每个转换线程直接参与 group statistics”作为独立的 D3D11 shader 优化，继续使用拓扑 A。

转换和分析必须保留两个 shader variant：

- 常规帧：只转换；
- 分析帧：转换并输出 group statistics。

分析间隔继续保持 `1/4`，除非基准数据显示可以在预算内提高频率。不得仅因为 D3D12 可用就默认逐帧分析。

### 7.3 编译与发布

- 使用 DXC 在构建阶段编译 `cs_6_0` 或经能力矩阵批准的更高 profile；
- CI 将 DXIL blob 嵌入二进制；
- 运行时不分发或加载 `dxcompiler.dll`；
- 保留当前 FXC/SM5 D3D11 shader 作为回退；
- CI 必须同时验证 DXIL 编译和 D3D11 shader 编译，防止两套实现漂移。

## 8. 编码后端接入顺序

### 8.1 NVENC

NVENC 是首个原生 D3D12 编码目标。SDK 支持 D3D12 input/output resource，并通过 `NV_ENC_FENCE_POINT_D3D12` 等待输入、通知资源可复用。

实施要求：

- 新增 `nvenc_d3d12`，不要修改现有 `nvenc_d3d11` 的行为；
- D3D12 session、resource registration 和 fence 生命周期封装在新类中；
- 保留当前 SDK 版本动态兼容策略；
- 保持强制 IDR、LTR、码率重配置、异步输出和 codec 参数行为一致；
- HDR Vivid/HDR10+ 的 SEI/OBU 注入仍在 codec/packet 层完成，不与图形 API 耦合；
- D3D12 初始化或注册失败时，销毁半成品并重试现有 D3D11 NVENC。

不采用 D3D12 Video Encode 替代 NVENC。原生 D3D12 Video Encode 从 Windows 11/WDDM 3.0 起提供 H.264/HEVC，AV1 又要求 Windows 11 24H2/WDDM 3.2；它会缩小 Sunshine 的系统与驱动兼容面。

### 8.2 AMF

AMF 在 NVENC 稳定后实施：

- 新增 `amf_d3d12`，保持 `amf_d3d11` 不变；
- 运行时探测 AMF DX12 初始化和输入表面支持；
- 验证 P010/NV12、HEVC/AV1、分辨率重配置和设备恢复；
- 逐项比对 AMF 属性设置，避免 D3D12 后端遗漏 bitrate、VBV、LTR、profile 或 HDR 静态元数据；
- 动态元数据仍由 Sunshine bitstream 层注入，不依赖 AMF 自动插入。

### 8.3 QSV/FFmpeg

首轮保持 D3D11：

- D3D12 compute 输出通过受控共享/复制交给现有 D3D11 encoder surface；
- 记录额外 copy 和 fence 成本；
- 只有当该成本在 Intel 平台构成明确瓶颈，并且 FFmpeg/QSV 的 D3D12 硬件帧链路满足稳定性和格式要求时，才立项 M6。

“API 存在”不等于“Sunshine 的编码器组合能稳定零拷贝”。M6 必须先做两周内可结束的技术验证，再决定继续或停止。

## 9. 配置与发布策略

### 9.1 开关范围与命名

新增一个 Windows 专用的持久配置：

```text
windows_video_backend = auto|d3d11|d3d12
```

默认值为 `auto`。代码中使用同名枚举，不在各模块传播字符串。

这个开关控制的是 Windows 视频处理和编码器资源交接后端，不是捕获 API：

- WGC、Desktop Duplication 和当前 VDD 捕获仍然可以是 D3D11；
- `d3d12` 表示在已实现且已验证的阶段使用 D3D12 compute、shared fence 或原生 D3D12 encoder surface；
- 若某编码器尚未支持原生 D3D12，实际链路可以是有明确日志的 hybrid；
- UI 名称使用“Direct3D 11/12”，避免把更宽泛的“DirectX 11/12”与捕获方式混为一谈。

它与现有设置的关系如下：

| 设置 | 职责 | 与新开关的关系 |
|---|---|---|
| `capture` | 选择 WGC/DDAPI/VDD 等捕获方式 | 正交，不被修改 |
| `encoder` | 选择 NVENC/AMF/QSV/软件编码器 | 正交；决定可用的 D3D12 末端 |
| `capture_compute_shader` | 当前 D3D11 compute/PS 转换选择 | 只影响 D3D11 路径和 D3D12 失败后的回退，不复用为 API 开关 |
| `windows_video_backend` | 选择视频处理/资源交接 API 策略 | 新增，本节定义 |

长期可以在 D3D11 compute 成为稳定默认后废弃 `capture_compute_shader`，但不能在 D3D12 项目中顺带删除，以免扩大迁移范围。

### 9.2 三种模式的语义

| 配置值 | 选择规则 | 初始化失败 | 运行期失败 |
|---|---|---|---|
| `auto` | 仅在能力、allow-list 和阶段性能门槛全部通过时选择 D3D12，否则 D3D11 | 静默回退并记录结构化原因 | 最多执行一次 D3D11 pipeline rebuild |
| `d3d11` | 跳过 D3D12 probe 和资源创建 | 不适用 | 不升级到 D3D12 |
| `d3d12` | 强制尝试本构建已交付的最高 D3D12 阶段 | 为保证串流可用，默认回退 D3D11并发出 warning | 最多执行一次 D3D11 pipeline rebuild |

`d3d12` 不是“所有模块必须是 D3D12”。例如首个 NVENC 阶段可能是：

```text
D3D11 capture + D3D11 conversion + shared D3D12 P010 + NVENC D3D12
```

日志必须同时显示配置模式和各组件的有效模式，不能只输出一个容易误解的 `D3D12 enabled`。

开发和 CI 需要严格失败来暴露误回退，使用进程级诊断变量：

```text
SUNSHINE_WINDOWS_VIDEO_BACKEND=d3d12
SUNSHINE_WINDOWS_VIDEO_BACKEND_STRICT=1
```

`STRICT=1` 只在显式请求 `d3d12` 时生效：初始化或运行期 D3D12 失败后终止该视频 pipeline，而不是回退。它不进入配置文件或 Web UI，也不作为普通用户支持接口。

### 9.3 配置优先级与生命周期

有效值按以下优先级解析：

1. 进程环境变量 `SUNSHINE_WINDOWS_VIDEO_BACKEND`；
2. CLI/配置文件解析后的 `windows_video_backend`；
3. 默认值 `auto`。

无效值输出一次 warning 并按 `auto` 处理。环境变量用于 CI、开发和应急排障；持久配置用于用户选择。

后端在视频会话创建时解析并锁定：

- 活跃串流期间修改配置，只影响下一次 pipeline 创建；
- 不允许在正常帧边界从 D3D11 热切换到 D3D12；
- D3D12 fatal error 只允许单向重建为 D3D11 一次；
- 重建必须销毁旧 encoder session、清空旧 generation、创建新的资源 ring，并强制输出 IDR；
- 如果 D3D11 重建也失败，按现有视频 pipeline 错误处理结束串流，禁止循环重试。

一次 pipeline 的选择状态机：

```text
resolve requested mode
  |
  +-- d3d11 ------------------------------> effective=d3d11
  |
  +-- auto/d3d12
        |
        +-- build stage unavailable ------> d3d11 / strict failure
        +-- capability probe failed ------> d3d11 / strict failure
        +-- topology self-test failed ----> d3d11 / strict failure
        +-- encoder path unavailable -----> hybrid or d3d11 / strict failure
        +-- all required gates passed ----> effective=d3d12 or hybrid
```

### 9.4 日志与状态

会话开始只输出一条稳定、可机器解析的选择日志：

```text
[video_backend] requested=d3d12 effective=hybrid capture=d3d11
  conversion=d3d11 analysis=d3d12 encoder=nvenc_d3d12
  adapter_luid=... fallback=none strict=false
```

回退时输出一条 warning：

```text
[video_backend] fallback from=d3d12 to=d3d11
  stage=encoder_init reason=nvenc_d3d12_register_failed hresult=...
```

`reason` 使用稳定枚举，至少覆盖：

- `build_stage_unavailable`
- `d3d12_device_failed`
- `shader_model_unsupported`
- `shared_resource_failed`
- `shared_fence_failed`
- `topology_benchmark_rejected`
- `encoder_backend_unavailable`
- `encoder_resource_registration_failed`
- `device_removed`
- `runtime_fence_failed`

性能覆盖层和诊断 API 显示 `requested`、`effective` 和各组件后端；HDR Vivid 标识仍只表示动态元数据实际生成/发送成功，不能把“选择了 D3D12”当作 Vivid 生效条件。

### 9.5 Web UI 推进

M1/M2 开发期只接受 raw config、CLI 和环境变量，不立即修改 Web UI。G1 通过后，在高级设置加入：

- `自动（推荐）`
- `Direct3D 11（兼容模式）`
- `Direct3D 12（实验性/高性能）`

旁边显示最近一次会话的有效后端与回退原因。设置修改标记为“下次串流生效”，不提供会话内即时切换，也不增加 per-app 或 per-client 覆盖。

当 G2 完成且 `auto` 已成为稳定默认后，移除“实验性”文案；保留 D3D11 选项作为兼容和紧急回退手段。

### 9.6 灰度

```text
disabled
  -> developer opt-in
  -> nightly opt-in
  -> auto on allow-listed capabilities
  -> broader auto with telemetry guard
  -> default path
```

每次扩大灰度前，必须通过相应阶段的退出门槛。驱动版本黑名单只能作为短期止血措施，并需要记录供应商、版本范围、失败原因和移除条件。

## 10. 可观测性

### 10.1 M0 是否必要

结论：M0 必要，但只做到“足以决定是否继续投资”的最低程度，不建设通用遥测平台。

必要性来自三个当前事实：

- `SUNSHINE_VRAM_TIMING` 的 CPU submit 计时在 `dispatch_hdr_analysis()` 前结束，不能代表完整 HDR 分析提交成本；
- #869 后 D3D11 已经有 compute 转换、缩放、cell-statistics snapshot 和两阶段 reduction，D3D12 可获得的剩余收益可能只有亚毫秒；
- 完整 D3D12 compute 可能新增 RGBA16F bridge copy，不先取得当前各阶段成本就无法判断净收益。

M0 是性能结论、M2/M3 投资决策和 `auto` 启用的前置条件，但不是编写 M1 device/fence/resource-ring 骨架的阻塞条件。M0 与 M1 可以并行；M0 结果未完成前不得宣称收益，也不得决定默认启用。

### 10.2 必须做到的程度

M0 timebox 为 4–6 人日，并限制为四项交付。

第一，修正测量边界并加入异步 GPU timestamp：

- `capture_or_bridge_copy_gpu_ms`
- `convert_gpu_ms`
- `encoder_surface_copy_gpu_ms`
- `analysis_pass1_gpu_ms`
- `analysis_pass2_gpu_ms`
- `analysis_readback_copy_gpu_ms`
- `video_submit_cpu_ms`

`video_submit_cpu_ms` 从开始处理捕获帧计到该帧所有 GPU work 提交完成，包含 HDR analysis dispatch。timestamp/query 采用 ring 异步回收，禁止为计时调用 blocking `GetData()`、`Map()` 或 `Flush()`。

第二，加入解释结果所需的最少计数器：

- `conversion_path=ps|d3d11_compute_direct|d3d11_compute_scratch`
- `analysis_due`
- `analysis_dispatched`
- `analysis_result_age_frames`
- `analysis_skipped_busy`
- `analysis_readback_not_ready`
- `capture_backend`
- `encoder_backend`

第三，输出一个会话摘要：

- 每个 timing 的 sample count、P50、P95、P99；
- 分析帧与非分析帧分开汇总；
- 路径选择计数和 fallback 原因；
- adapter、driver、分辨率、刷新率、捕获、encoder、codec、HDR transfer function；
- 可由脚本保存为一个 machine-readable JSON；M0 结项时再人工整理一页 Markdown 对比表，不为 CSV 或报表生成器增加工作量。

默认构建不逐帧写日志。详细 timing 仍由 `SUNSHINE_VRAM_TIMING=1` 显式启用，会话结束或固定长间隔输出聚合结果。

第四，采集一个有限但可复现的当前 D3D11 基线：

- NVIDIA、AMD、Intel 各一张代表性 GPU；M0 缺少某家设备时允许先做两家，但缺失项必须在 G1 前补齐；
- 主矩阵为 4K60、4K120、1:1 和一个常用缩放场景；
- HDR analysis off/on；
- D3D11 compute direct、compute scratch 和 PS fallback 中实际可达的路径；
- VDD 主路径完整测试，WGC/DDAPI 各做一次 timing 可用性 smoke test；
- 每个主场景预热后至少记录 2,000 帧并重复 3 次。

重复测试的 P95 偏差目标不超过 `max(10%, 0.05 ms)`。另做一次 instrumentation off/on A/B；若计时本身的平均 CPU 增量超过 0.02 ms 或使 GPU frame time 增加超过 0.5%，必须先降低采样频率或简化指标。

### 10.3 M0 明确不做

- 不上传用户遥测；
- 不建设数据库、dashboard 或 Web UI 图表；
- 不覆盖全部 GPU 代际和驱动版本；
- 不做 24 小时 soak、设备丢失恢复或完整 codec 认证；
- 不实现 D3D12 后端；
- 不为了测量重构整个 `display_vram.cpp`；
- 不把诊断计数永久放进性能覆盖层。

这些工作分别属于 M1、G1/G2 或发布阶段。

### 10.4 M0 退出与停止条件

M0 报告必须回答：

1. 当前 D3D11 conversion、analysis 和 surface copy 的 P50/P95/P99 各是多少；
2. 分析帧相对非分析帧增加多少，1/4 采样后的平均成本是多少；
3. direct、scratch 和 PS fallback 各占多少；
4. readback 是否造成 metadata age 或周期性未就绪；
5. 理论上可由 D3D12 消除的成本上限是多少。

决策规则：

- 若 analysis on/off 的 P95 差异小于测量噪声，则不把 D3D12 analysis 作为独立性能项目，只保留正确性/架构验证；
- 若主要成本是 `encoder_surface_copy_gpu_ms`，优先拓扑 A 和原生 D3D12 encoder surface；
- 若主要成本是 conversion，且 RGBA bridge copy 的带宽模型已经接近或超过可回收成本，则 M1 必须证明低复制输入拓扑，否则停止完整 D3D12 compute；
- 若主要问题是 tail latency 或 readback age，而不是平均 GPU 时间，则 M2 优先解决异步队列和 slot 状态机；
- 4–6 人日结束时已有可信基线即结束 M0，不为“更漂亮的工具”延期。只有测量侵入性过高或结果不可重复时才允许延长。

### 10.5 M1 之后再增加的指标

D3D12 后端出现后复用 M0 的聚合器，并按实现阶段增加：

- `requested_video_backend`
- `effective_video_backend`
- `d3d12_fallback_reason`
- `bridge_copy_gpu_ms`
- `compute_queue_wait_gpu_ms`
- `encode_wait_gpu_ms`（编码器能够提供时）
- `resource_ring_high_watermark`
- `device_removed_reason`

这些不是 M0 的交付内容。D3D11、D3D12 analysis-only、fused conversion 和原生 encoder input 必须使用同一测量口径，否则不能横向比较。

## 11. 分阶段实施与退出门槛

| 阶段 | 主要交付 | 人日估算 | 退出门槛 |
|---|---|---:|---|
| M0 决策基线 | 最小 timing、路径计数、可复现实验脚本和基线报告 | 4–6 | 回答第 10.4 节五个问题；结果可重复且测量开销达标 |
| M1 D3D12 基础 | device、compute queue、fence、3-slot ring、离线 DXIL、两种共享拓扑自测 | 10–15 | 选定低复制拓扑；失败 100% 回退；debug layer clean |
| M2 分析等价 | D3D12 两阶段分析、异步 readback、golden tests | 12–18 | 统计一致；24 小时无资源泄漏/卡死 |
| G1 分析验收 | 跨 GPU/OS/分辨率 QA 与 hardening | 8–12 | 满足第 13 节分析阶段门槛 |
| M3 融合转换（条件阶段） | D3D12 RGB->P010/NV12、缩放、直接 group statistics | 15–25 | 复制成本门控通过；画质、统计、边界和格式全部等价 |
| M4 NVENC | 原生 D3D12 输入、显式 fence、回退 | 12–18 | H.264/HEVC/AV1 功能与码流行为无回归 |
| G2 NVIDIA 验收 | 性能、视觉、24h soak、设备恢复 | 19–26 | 满足第 13 节完整链路门槛 |
| M5 AMF + AMD 验收 | AMF DX12、属性一致性、跨代 AMD QA | 35–45 | HEVC/AV1、重配置和恢复通过 |
| M6 QSV/FFmpeg | 先技术验证，再按收益决定实现 | 30–50 | 仅在收益覆盖维护成本时合并 |

阶段不可用“后续会补测试”作为退出条件。M0–M2 可以先形成独立价值；M3–M4 是主要性能目标；M5、M6 是兼容面扩展。

### 11.1 PR 拆分

建议 PR 边界：

1. timing 与 benchmark harness；
2. `windows_video_backend` 解析、选择日志和状态机，不创建 D3D12 资源；
3. D3D12 device/shared fence/resource ring，默认不被 `auto` 选择；
4. D3D12 HDR analysis parity；
5. SM6 wave reduction；
6. D3D12 fused conversion；
7. NVENC D3D12；
8. AMF DX12；
9. Web UI、自动选择和灰度策略。

单个 PR 不同时引入新同步模型、新 shader 算法和新编码器后端。每个 PR 都包含对应的回退、日志和测试。

## 12. 成本与资源预算

### 12.1 工程成本

以下按 Windows GPU/视频方向工程师 fully-loaded 成本 ¥2,500–4,000/人日估算，只用于立项规划，不是交付报价：

| 可交付范围 | 累计人日 | 预算区间 | 建议团队与日历周期 |
|---|---:|---:|---|
| M0–G1：分析后端可用 | 34–51 | ¥85,000–204,000 | 1 名资深工程师，7–10 周 |
| 加 M3–G2：转换 + NVENC | 80–120 | ¥200,000–480,000 | 2 名工程师，12–18 周 |
| 加 M5：NVIDIA + AMD | 115–165 | ¥287,500–660,000 | 2 名工程师，18–26 周 |
| 加 M6：再覆盖 QSV/FFmpeg | 145–215 | ¥362,500–860,000 | 2–3 名工程师，24–36 周 |

日历周期已经考虑代码评审、驱动差异和测试矩阵，不能简单用人日除以人数。若只有一套 GPU 测试机或缺少自动化画质对比，应额外预留 20%–30%。

### 12.2 4K 资源预算

计算公式：

```text
bytes = width * height * bytes_per_pixel * slots
```

3840x2160 单 slot 约为：

| 资源 | 字节/像素 | 单 slot |
|---|---:|---:|
| RGBA16F | 8 | 63.3 MiB |
| R32 sidecar | 4 | 31.6 MiB |
| R16 sidecar | 2 | 15.8 MiB |
| P010 | 约 3 | 23.7 MiB |
| NV12 | 约 1.5 | 11.9 MiB |

全帧复制还必须按带宽核算。4K60 的 RGBA16F 逻辑复制量约为 3.71 GiB/s，读加写的显存流量约为 7.42 GiB/s；4K120 加倍。P010 在 4K60 下分别约为 1.39 GiB/s 和 2.78 GiB/s。因此，“增加一份 RGBA 输入复制、消除一份 P010 输出复制”通常不是天然收益，必须以 GPU timestamp 证明。

目标实现不保留全分辨率 R32/R16 sidecar，而由转换 dispatch 直接输出小型 group results。主要新增显存预算是：

- 3-slot P010 ring：约 71.2 MiB；
- 或 3-slot NV12 ring：约 35.6 MiB；
- shared capture bridge：若必须单独复制，RGBA16F 每 slot 63.3 MiB，应优先复用/双缓冲并通过实测决定 slot 数；
- D3D12 device、descriptor heaps、PSO、driver allocations：预估 10–30 MiB；
- group results、histogram 和 readback：相对全帧纹理很小。

分析阶段的新增显存硬门槛为 4K 不超过 128 MiB。若跨 API 限制迫使 3 份额外 RGBA16F 副本常驻，M1 必须重新设计资源共享或减少 bridge slots，不直接放行。

NVENC 文档建议 D3D12 output buffer 按输入 YUV 大小的 2 倍分配。4K P010 约 47.5 MiB/输出 slot，3 slot 约 142 MiB，通常属于 readback/system-visible 预算，仍需单独记录，不能混入“显存几乎不增加”的表述。

## 13. 性能预期与验收标准

### 13.1 性能预期

以下是工程估算，必须由 M0 基准替换：

| 场景 | 4K 预期 |
|---|---:|
| 当前 D3D11 HDR 分析，按 1/4 帧摊销 | 平均约 +0.2–0.7 ms；分析帧尖峰约 +0.8–2.8 ms |
| D3D12 analysis-only | 分析平均约 0.08–0.35 ms |
| D3D12 fused conversion + analysis 的分析增量 | 现代独显约 0.05–0.2 ms/帧 |
| 完整转换/交接链路收益 | 约 0.3–1.2 ms；1080p 约 0.05–0.3 ms |

D3D12 不是天然更快。在 GPU 已饱和时，async compute 可能与游戏渲染争抢 CU，造成尾延迟上升。自动启用必须同时看 Sunshine 处理时间、游戏帧时间和 dropped frames。

### 13.2 正确性门槛

- 同一输入序列下，min/max 必须覆盖完全相同的编码像素集合；
- average 误差不超过预先记录的 FP 舍入容差；
- 256 桶 PQ 直方图总计数等于参与分析的像素数；
- P10/P90/P95/P99 不得因 API 切换产生周期性跳变；
- PQ、HLG、缩放、letterbox/pillarbox、奇数 active rect 均有 golden case；
- HDR Vivid 与 HDR10+ 码流元数据字段保持一致；
- 不出现新的亮度忽明忽暗、场景切换闪烁或元数据 age 振荡。

### 13.3 性能与稳定性门槛

进入自动灰度前必须满足：

- 4K HDR analysis 场景 P95 host processing 改善至少 20%，或绝对改善至少 0.2 ms；
- dropped frame 比例回归不超过 0.1 个百分点；
- CPU submit 平均值回归不超过 0.05 ms；
- 游戏侧 frame-time P99 无统计显著回归；
- D3D12 analysis-only 新增显存不超过 128 MiB；
- 24 小时持续串流无 deadlock、资源泄漏或 fence 停滞；
- 分辨率/HDR/codec 切换 500 次无迟到 generation 被发布；
- 强制能力失败、设备丢失和 encoder 初始化失败时回退成功率 100%。

若 M2 达到正确性但没有达到性能门槛，保留代码作为实验后端，不进入自动灰度；若 M3 仍无可测收益，则停止 M4/M5，而不是为了完成“全 D3D12”继续扩大改动。

## 14. 测试矩阵

最低矩阵：

| 维度 | 覆盖 |
|---|---|
| GPU | NVIDIA Ampere/Ada 及更新；AMD RDNA2/RDNA3 及更新；Intel Xe/Arc |
| OS | Windows 10 22H2；Windows 11 23H2/24H2 及当前支持版本 |
| 捕获 | WGC、Desktop Duplication、VDD sealed channel |
| 编码 | NVENC、AMF、QSV/FFmpeg fallback |
| codec | H.264、HEVC、AV1（后端支持时） |
| 格式 | SDR NV12、HDR PQ P010、HDR HLG P010 |
| 分辨率 | 1080p、1440p、4K；原尺寸和非整数缩放 |
| 帧率 | 60、120；有条件时 240 fps |
| 生命周期 | 启停、分辨率切换、HDR 切换、codec 切换、显示器重建、休眠恢复 |

自动化包括：

- CPU reference 与 D3D11/D3D12 statistics golden comparison；
- P010/NV12 frame dump 的 PSNR/SSIM 和边界像素检查；
- SEI/OBU 提取与字段比较；
- fence slot 状态机单元测试；
- shader 离线编译测试；
- D3D12 debug layer；
- 测试构建启用 DRED，保存 device removal breadcrumbs/page fault 信息。

## 15. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| 跨 API shared texture/fence 驱动差异 | 卡死、复制失败 | 启动自测、同 LUID、超时/错误即会话级回退 |
| async compute 与游戏争抢 GPU | 游戏 P99 变差 | 低频分析、telemetry gate、忙时跳过、可禁用 |
| D3D11/D3D12 两套 shader 漂移 | 颜色或元数据不一致 | 共用公式 include、golden tests、双编译 CI |
| ring slot 提前复用 | 闪烁、数据竞争、device removal | 单一状态机、单调 fence、debug assertions |
| 原生 encoder 后端功能缺失 | 编码行为回归 | 新类并行存在、逐属性对照、先 NVENC 后 AMF |
| 显存增幅过大 | 低显存设备抖动 | 预算门槛、按格式分配、分辨率变化后释放旧 generation |
| 维护面过大 | 长期成本失控 | 阶段停止条件、QSV 延后、无收益即不扩展 |

## 16. 里程碑决策点

### G1：是否继续做转换融合

继续条件：

- D3D12 分析正确、稳定；
- 至少两家 GPU 的 P95 有明确改善；
- bridge 显存和同步成本在预算内。

否则保持 D3D11 默认，关闭后续实施。

### G2：是否接入更多编码器

继续条件：

- fused conversion 相对当前 D3D11 compute path 仍有明确收益；
- NVENC D3D12 没有破坏 HDR 动态元数据和低延迟行为；
- 维护复杂度没有迫使公共视频层暴露平台细节。

AMF 通过后再决定 QSV。QSV 的决策必须基于 Intel 平台的 copy 成本数据，不以架构对称性为理由。

## 17. 官方参考

- [Microsoft: Direct3D 11 on 12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12)
- [Microsoft: Executing and synchronizing command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists)
- [Microsoft: HLSL Shader Model 6.0 wave operations](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/hlsl-shader-model-6-0-features-for-direct3d-12)
- [Microsoft: ID3D11Fence::CreateSharedHandle](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle)
- [NVIDIA: NVENC Video Encoder API Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html)
- [AMD: Advanced Media Framework releases](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases)
- [FFmpeg: AVD3D12VADeviceContext](https://ffmpeg.org/doxygen/7.0/structAVD3D12VADeviceContext.html)
- [Microsoft: D3D12 video encoding](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/video-encoding-d3d12)
- [Microsoft: D3D12 AV1 video encoding](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/video-encoding-d3d12-av1)
