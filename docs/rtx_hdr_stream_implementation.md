# RTX HDR 串流完整实施方案

## 1. 文档状态

- 目标仓库：Foundation Sunshine
- 目标平台：Windows x64
- 首期 GPU：NVIDIA GeForce RTX 20 系列及更新架构
- 首期图形 API：D3D11
- 输入：Rec.709 / sRGB SDR 桌面帧
- 内部 HDR 交换格式：`DXGI_FORMAT_R16G16B16A16_FLOAT` linear scRGB
- 输出：BT.2020 + PQ + HEVC Main10 / AV1 10-bit
- 动态元数据：复用现有 HDR10+、HDR Vivid、Dolby Vision Profile 8.1 管线
- 默认状态：关闭；按应用显式启用

本文定义可直接拆分为工程任务和 Pull Request 的实施方案。除 NVIDIA 后端外，
所有新增的主机接口均使用厂商无关命名，以便以后接入其他 SDR→HDR 算法。

---

## 2. 核心决策

### 2.1 RTX HDR 是编码前滤镜，不是捕获后端

不尝试捕获 NVIDIA App 或驱动注入后的 RTX HDR 画面。Sunshine 自己捕获 SDR 帧，
然后在编码前调用 NVIDIA RTX Video SDK TrueHDR：

```text
SDR 应用 / SDR 桌面
        ↓
DXGI / WGC / Zako VDD Direct Capture
        ↓
规范化 SDR 私有纹理（BGRA8_UNORM, Rec.709/sRGB）
        ↓
Pre-encode Filter Graph
        ↓
NVIDIA TrueHDR
        ↓
FP16 linear scRGB（1.0 = 80 nits）
        ├──→ 现有亮度分析 → HDR10+ / Vivid / Dolby Vision 元数据
        └──→ 现有 RGB→PQ/P010 → NVENC/AMF/QSV 编码接口
```

RTX HDR 不拥有捕获生命周期、编码器生命周期或 RTSP 协商；它只实现一个明确的
GPU 纹理变换契约。

### 2.2 源动态范围与输出动态范围必须拆开

当前代码多处用 `video::config_t::dynamicRange` 同时表示：

1. 客户端和编码器是否使用 HDR；
2. 主机显示器是否开启 HDR；
3. WGC 应请求 BGRA8 还是 FP16。

TrueHDR 场景中三者不再相等：

| 属性 | TrueHDR 会话值 |
|---|---|
| 客户端/码流动态范围 | HDR PQ |
| 主机源显示器动态范围 | SDR |
| 捕获纹理 | SDR UNORM |
| TrueHDR 输出纹理 | FP16 linear scRGB HDR |

因此会话策略必须分别解析三份结果：

```text
stream_output_contract   客户端/编码器应该输出什么
source_display_intent    主机源显示器应该处于什么状态
capture_contract         捕获层需要交付什么语义的帧
```

`synthetic_hdr` 只是生成这三份结果的一种上层策略输入，不能直接传入 WGC、DXGI 或
VDD。捕获后端不得出现 `if (rtx_hdr)`、`if (truehdr)` 或 NVIDIA 类型。

### 2.3 捕获后端只认识通用帧契约

捕获层与 RTX HDR 的唯一关系是通用输入契约。策略层可以因为 TrueHDR、录像、截图或
未来其他滤镜而提出同一种要求：

```text
交付 Rec.709/sRGB SDR
优先使用 8-bit UNORM
处理前必须脱离 producer 借用生命周期
保留 adapter 和同步信息
```

捕获后端只消费 `capture_contract_t`，并返回 `captured_frame_desc_t`。编码前规范化层检查
“实际交付”是否满足“请求契约”，TrueHDR 只消费规范化结果。这样新增捕获后端无需引用
任何 RTX HDR 文件，新增后处理算法也无需修改所有捕获后端。

### 2.4 插件边界使用稳定 C ABI

主程序继续使用当前 MinGW 工具链。NVIDIA SDK 后端单独用 MSVC 编译为：

```text
tools/rtx_hdr/foundation_truehdr_backend.dll
tools/rtx_hdr/nvngx_truehdr.dll
```

主程序通过版本化 C ABI 加载后端。不得跨 DLL 边界传递 STL、异常、C++ 对象所有权
或编译器相关结构。允许传递 ABI 稳定的 D3D11 COM 接口指针。

### 2.5 主机拥有输入和输出纹理

后端不得返回生命周期不透明的内部纹理。Sunshine 创建并持有规范化 SDR 输入纹理和
FP16 输出纹理，再把两者交给后端处理。这样可以：

- 明确资源生命周期；
- 直接复用输出 SRV/UAV；
- 无缝接入亮度分析和编码；
- 后端失败后安全重建；
- 避免下一次 SDK 调用使借用指针失效。

### 2.6 会话级降级，不做逐帧画质切换

不允许在 TrueHDR 和普通 SDR→PQ 之间逐帧来回切换。初始化未完成时使用稳定的中性
SDR→PQ；TrueHDR 连续成功后只在可控边界激活；连续失败后本会话永久降级，直到下次
重建视频会话。

### 2.7 术语链：rtx_hdr / synthetic_hdr / pre_encode_filter 各指什么

同一特性在代码里有三个名字，分别属于三层，**只能被各自所在层消费**，不能拿相邻层
的值做另一层的判断（2026-09 曾因此产生 8.4 协商误判，见 profile84.md §2）：

| 名字 | 所在层 | 生命周期 | 消费者 |
|---|---|---|---|
| `rtx_hdr`（apps.json 配置键） | 用户配置 | 持久 | nvhttp：填充 `session.synthetic_hdr` |
| `synthetic_hdr.enabled`（`launch_session_t`） | 会话意图 | ANNOUNCE 时即可读 | 协商层 gate `synthetic_hdr_enabled`；`post_process_hdr_active` 的输入 |
| `pre_encode_filter`（`video::config_t`）+ `post_process_hdr_active`（本地计算） | 管线对象 | 编码会话期间 | display_vram 的 filter 构造、colorspace/metadata 决策 |

边界规则（均有单测钉住）：

- HLG 守卫钉 PQ：`post_process_hdr_active = synthetic_hdr.enabled && dynamicRange == 1`，
  因此 **HLG 会话里 filter 对象恒不存在**；
- 8.4 协商排除跟随**应用意图**（`synthetic_hdr.enabled` → gate
  `synthetic_hdr_enabled`），不跟随会话 filter 状态 —— 否则 RTX HDR 应用 + HLG
  客户端会漏排除；
- 协商 gate 位于 `dynamic_hdr_host_gates_t`（dynamic_hdr_selection.h），消费规则
  见 profile84.md §2、§3.1。

---

## 3. 范围

### 3.1 首期必须完成

- Windows x64、D3D11、NVIDIA RTX GPU；
- WGC、DXGI Desktop Duplication、Zako VDD Direct Capture；
- HEVC Main10 和 AV1 10-bit；
- 4:2:0 P010；
- 全局配置与单应用启用；
- TrueHDR 参数快照：对比度、饱和度、中灰、峰值；
- 与客户端亮度能力、HDR10+、Vivid、Dolby Vision 8.1 的一致性；
- 多会话安全串行化；
- 运行状态、耗时、失败原因和 Web UI 展示；
- 后端缺失、不支持、超时、设备丢失时的安全降级；
- 可选组件的构建、签名、固定版本、完整性验证和卸载。

### 3.2 首期明确不做

- 捕获 NVIDIA App 已注入的 RTX HDR；
- NVIDIA DRS/Profile Inspector 自动读取或写入；
- 未公开的 D3D11 VideoProcessor 扩展 GUID；
- D3D12、CUDA 或 Vulkan TrueHDR 后端；
- 4:4:4 TrueHDR 首发保证；
- 运行中无重建地切换 SDR/HDR 编码 profile；
- AMD/Intel 的 AI SDR→HDR；
- 2000-nit 后缩放补偿；首期峰值限制为经过验证的 400–1000 nits；
- 独立进程隔离。若实测证明 NGX 调用可永久挂起，再进入第二阶段。

---

## 4. 当前主线基础与缺口

### 4.1 可以直接复用

当前 `src/platform/windows/display_vram.cpp` 已具备：

- D3D11 共享纹理和 keyed mutex 生命周期；
- BGRA8 与 FP16 scRGB 输入；
- pixel shader 和 compute shader RGB→NV12/P010；
- `hdr_pre_encode_transform`；
- FP16 HDR 亮度分析、直方图和场景元数据；
- NVENC、AMF 等编码输出适配；
- `video::hdr_pipeline_status_t` 运行状态。

当前 `src/video.cpp` 已具备：

- BT.2020/PQ/HLG 色彩声明；
- MDCV、CLL 静态元数据；
- HDR10+、HDR Vivid 动态元数据；
- Dolby Vision Profile 8.1 RPU；
- 客户端目标亮度映射。

### 4.2 必须补齐

1. 缺少独立于输出 `dynamicRange` 和具体后处理算法的 `capture_contract_t`；
2. WGC 目前在 `dynamicRange != 0` 时固定请求 FP16，无法消费独立捕获契约；
3. 自动显示准备目前根据客户端 HDR 请求打开主机 HDR，缺少独立的
   `source_display_intent`；
4. `video_colorspace.cpp` 依赖 `display.is_hdr()` 才建立 HDR colorspace，TrueHDR 需要把
   合成 HDR 视为有效 HDR 源；
5. 静态 HDR 元数据当前优先从被捕获显示器读取；源显示为 SDR 时必须从会话目标能力
   合成；
6. 捕获结果只携带零散的 `format/linear_gamma` 信息，缺少完整的色彩、adapter、同步和
   所有权描述；
7. HDR 分析当前判断依据是原始 `img`，必须改为分析 TrueHDR 的 FP16 输出；
8. 缺少厂商无关的编码前 GPU filter 接口；
9. 缺少 NVIDIA SDK 后端、组件管理和能力探测；
10. 缺少 RTX HDR 会话状态和性能统计。

---

## 5. 会话模型与协商

### 5.1 新增会话字段

在 `src/rtsp.h::launch_session_t` 和 `src/video.h::config_t` 增加：

```cpp
enum class synthetic_hdr_backend_e : std::uint8_t {
  none,
  nvidia_truehdr,
};

struct synthetic_hdr_config_t {
  bool requested = false;
  bool active = false;
  synthetic_hdr_backend_e backend = synthetic_hdr_backend_e::none;
  int contrast = 100;       // SDK 单位 0..200
  int saturation = 100;     // SDK 单位 0..200
  int middle_gray = 50;     // 10..100
  int peak_nits = 1000;     // 首期 400..1000
};
```

`requested` 表示启动策略希望使用；`active` 只在能力探测和后端初始化成功后为真。
不得用保存配置推断运行状态。

同时在新的 `src/platform/frame_contract.h` 定义厂商无关契约：

```cpp
namespace platf {

  enum class frame_domain_e : std::uint8_t {
    unknown,
    sdr_rec709,
    linear_scrgb,
    pq_bt2020,
    hlg_bt2020,
  };

  enum class pixel_encoding_class_e : std::uint8_t {
    automatic,
    unorm8,
    float16,
  };

  enum class source_display_intent_e : std::uint8_t {
    unchanged,
    require_sdr,
    require_hdr,
  };

  struct capture_contract_t {
    frame_domain_e required_domain = frame_domain_e::unknown;
    pixel_encoding_class_e preferred_encoding =
      pixel_encoding_class_e::automatic;
    bool require_private_handoff = false;
  };

  struct captured_frame_desc_t {
    frame_domain_e domain = frame_domain_e::unknown;
    pixel_encoding_class_e encoding =
      pixel_encoding_class_e::automatic;
    float reference_white_nits = 0.0f;
    std::uint64_t adapter_luid = 0;
    bool borrowed = false;
    std::uint64_t source_generation = 0;
  };
}
```

平台通用契约不包含 `DXGI_FORMAT`、COM 指针或 keyed mutex。Windows 的
`img_d3d_t` 在上述语义字段之外继续保存实际纹理、DXGI format 和同步对象。

输出侧也使用独立的通用契约，避免继续把 `dynamicRange` 当作捕获格式开关：

```cpp
enum class wire_transfer_e : std::uint8_t {
  sdr,
  pq,
  hlg,
};

struct stream_output_contract_t {
  wire_transfer_e transfer = wire_transfer_e::sdr;
  bool require_10bit = false;
  bool require_bt2020 = false;
};
```

现有 `dynamicRange` 在过渡期由该契约单向派生，不能再被捕获层读取。迁移完成后，
`dynamicRange` 只保留为协议/编码器兼容字段，`stream_output_contract_t` 才是会话内事实源。

会话最终保存四类正交状态：

```cpp
video::config_t::dynamicRange                 // wire/output contract
video::config_t::synthetic_hdr                // filter policy/result
video::config_t::source_display_intent        // display preparation
video::config_t::capture_contract             // capture request
```

### 5.2 启动决策顺序

RTX HDR 决策必须发生在 `prepare_display_and_probe_encoders()` 之前：

```text
解析客户端 HDR 能力和 hdrMode
        ↓
读取全局 RTX HDR 配置
        ↓
读取 apps.json 当前应用覆盖
        ↓
确认客户端请求 HDR + HEVC/AV1 Main10 可用
        ↓
确认目标 adapter 是 NVIDIA + 后端组件存在
        ↓
解析 synthetic HDR 策略
        ↓
生成 stream_output_contract / source_display_intent / capture_contract
        ↓
显示准备只消费 source_display_intent
        ↓
捕获后端只消费 capture_contract，并报告 captured_frame_desc
        ↓
规范化层校验 captured_frame_desc，后端 capability + feature create 成功
        ↓
config.synthetic_hdr.active = true
```

如果客户端没有请求 HDR，不得因为应用配置启用了 RTX HDR 而向客户端发送 10-bit HDR。
这避免把 Main10/PQ 码流交给只建立了 SDR 解码器的旧客户端。

### 5.3 显示准备

显示准备层不得检查 `synthetic_hdr`。策略解析器提前把结果写成
`source_display_intent`，`src/display_device/parsed_config.cpp::parse_hdr_option()` 只消费
这个通用意图：

```cpp
switch (session.source_display_intent) {
  case platf::source_display_intent_e::require_sdr:
    return false;
  case platf::source_display_intent_e::require_hdr:
    return true;
  case platf::source_display_intent_e::unchanged:
    return boost::none;
}
```

约束：

- 自动 HDR 准备模式下，TrueHDR 会话明确关闭目标显示 HDR；
- 用户选择“不操作 HDR”且显示器已经处于 HDR 时，能力探测返回
  `source_display_not_sdr`，本会话不得启用 TrueHDR；
- VDD 会话创建 SDR VDD，但码流仍按 HDR 建立；
- Vulkan HDR bridge 只在 `source_display_intent=require_hdr` 时启用；
- 会话结束仍沿用现有显示状态恢复机制。

### 5.4 输出 HDR 契约

一旦会话决定使用 synthetic HDR，整个视频会话保持：

- `dynamicRange = 1`（PQ）；
- 10-bit encoder profile；
- BT.2020 primaries；
- ST 2084 transfer；
- 与协商结果一致的动态 HDR 格式；
- 不因 TrueHDR 暂时未就绪而切换编码 profile 或 colorspace。

这里的输出信号不得再由 `display.is_hdr()` 决定。`display.is_hdr()` 只描述捕获源，不能描述
post-filter 帧。实现统一通过 `postprocess_produces_hdr_output(policy, filter)` 判断编码前滤镜是否
成为 HDR 源；色彩空间选择、静态 HDR metadata、NVENC/AMF 配置和客户端 HDR 事件都消费这个
结果。也就是说，TrueHDR 的合法状态本来就是：

```text
display.is_hdr() = false              # VDD/物理显示仍是 SDR
captured_frame.domain = sdr_rec709     # 捕获事实
post-filter frame = FP16 scRGB         # 后处理事实
wire output = PQ + BT.2020 + 10-bit    # 输出契约
```

曾经存在的残余耦合是 `video_colorspace.cpp` 和静态 metadata 路径仍以
`display.is_hdr()` 作为 HDR 门禁，结果 TrueHDR 像素已生成但编码器仍声明 SDR。该门禁现已移除：
原生 HDR 继续读取显示 metadata；合成 HDR 则按会话固定的峰值和 D65/BT.2020 生成 metadata，
不会向刻意保持 SDR 的 VDD 查询 HDR 能力。

---

## 6. 捕获格式改造

### 6.1 契约解析与边界

新增纯策略函数，不引用 D3D11 或 NVIDIA SDK：

```text
src/platform/frame_contract.h
src/platform/frame_contract.cpp
```

```cpp
struct frame_pipeline_policy_t {
  stream_output_contract_t output;
  platf::source_display_intent_e source_display;
  platf::capture_contract_t capture;
};

frame_pipeline_policy_t resolve_frame_pipeline_policy(
  int dynamic_range,
  bool post_process_hdr_active);
```

主要结果：

| 会话类型 | Wire output | Source display | Capture contract |
|---|---|---|---|
| 普通 SDR | SDR | SDR/unchanged | `sdr_rec709 + unorm8` |
| 原生 HDR | HDR PQ/HLG | HDR | `linear_scrgb + float16` |
| TrueHDR | HDR PQ | SDR | `sdr_rec709 + unorm8 + private_handoff` |

边界规则：

- RTSP/应用策略可以知道 TrueHDR，但只输出通用 contract；
- display session 只接收 `source_display_intent`；
- WGC/DXGI/VDD 只接收 `capture_contract_t`；
- capture backend 返回 `captured_frame_desc_t`，不得声称无法验证的色彩语义；
- normalization/filter 层负责比较 request 与 actual；
- `require_private_handoff` 是整条通用捕获管线的交付要求，不要求每个 backend 自己复制；
  backend 能直接交付私有纹理时可满足它，否则由 backend 之后的统一 handoff/normalization
  stage 复制，并在释放 borrowed slot 或 keyed mutex 后再进入耗时滤镜；
- 捕获层日志使用 domain/encoding/ownership，不打印 synthetic HDR backend 名称；
- RTX HDR 的诊断层可以把捕获契约和 filter 状态组合展示，但不能反向把厂商状态注入
  捕获后端。

### 6.2 WGC

修改 `src/platform/windows/display_wgc.cpp`：

```cpp
display->capture_format = select_wgc_format(config.capture_contract);
```

通用映射函数：

```cpp
DXGI_FORMAT select_wgc_format(const platf::capture_contract_t &contract) {
  switch (contract.preferred_encoding) {
    case platf::pixel_encoding_class_e::unorm8:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case platf::pixel_encoding_class_e::float16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case platf::pixel_encoding_class_e::automatic:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
  return DXGI_FORMAT_B8G8R8A8_UNORM;
}
```

日志必须同时输出：

```text
capture_required_domain=sdr_rec709
capture_preferred_encoding=unorm8
capture_actual_domain=sdr_rec709
capture_actual_format=B8G8R8A8_UNORM
capture_ownership=private
```

不得在 WGC 日志或类型中出现 `rtx_hdr`。`dynamicRange=1` 也不能再作为 WGC 格式选择
依据。

### 6.3 DXGI Desktop Duplication

显示器成功切换到 SDR 后，捕获应自然得到 BGRA/RGBA UNORM。若实际格式是 FP16：

- 不把 FP16 强制量化后送入 TrueHDR；
- 将原因记录为 `source_capture_not_sdr`；
- 保持 HDR wire contract，使用中性 SDR→PQ 降级，或在编码器创建前终止 synthetic HDR
  并按普通 HDR/SDR 能力重新探测；
- 首期选择“探测阶段禁用 synthetic HDR”，避免运行中重建。

### 6.4 Zako VDD Direct Capture

- display session 根据 `source_display_intent=require_sdr` 创建 SDR VDD；
- producer 应报告 BGRA/RGBA UNORM；
- borrowed texture 不直接交给 NGX；必须先复制/规范化到 Sunshine 私有输入槽，再释放
  VDD borrowed slot；
- 不允许 NGX 推理时间占用 VDD producer 的 keyed mutex。

VDD 后端只需报告纹理为 `borrowed=true`；通用 handoff stage 看到
`capture_contract.require_private_handoff=true` 后完成复制。VDD 不需要知道私有副本后面将被
TrueHDR、录像器还是其他滤镜使用。

#### 非 RTX HDR fast path 不变量

关闭 RTX HDR 且没有其他异步消费者时，必须保持当前 VDD 数据路径：

```text
borrowed/direct:
VDD shared texture → acquire keyed mutex → 现有 NV12/P010 convert
                   → release keyed mutex → encoder

copy fallback:
VDD shared texture → 现有 CopyResource 到 Sunshine capture texture
                   → 现有 NV12/P010 convert → encoder
```

不得因为引入 frame descriptor、filter graph 或通用 handoff 而无条件复制。分支只取决于
下游资源生命周期要求：

```cpp
const bool needs_detached_input =
  config.capture_contract.require_private_handoff ||
  filter_graph.requires_detached_input();

if (img.frame_desc.borrowed && needs_detached_input) {
  auto private_frame = handoff_and_release(img);  // copy/normalize 后立即释放 VDD slot
  return filter_graph.process(private_frame);
}

return convert_existing_path(img);  // 保留当前 borrowed/direct 或 copy fallback
```

`filter_graph.requires_detached_input()` 是会话级不可变 capability，不允许逐帧根据
TrueHDR 成功/失败状态切换，否则会造成资源槽、mutex 时序和帧延迟抖动。关闭 RTX HDR
时不得创建 handoff texture、normalized SDR texture、FP16 filter output 或 worker job。

### 6.5 捕获结果描述

Windows 捕获实现完成一帧后同时设置：

```cpp
img_d3d_t::frame_desc.domain;
img_d3d_t::frame_desc.encoding;
img_d3d_t::frame_desc.reference_white_nits;
img_d3d_t::frame_desc.adapter_luid;
img_d3d_t::frame_desc.borrowed;
img_d3d_t::frame_desc.source_generation;
```

映射依据必须来自实际 API 结果：

- WGC：请求的 pixel format、目标 display advanced-color 状态和实际 texture format；
- DXGI：`DXGI_OUTDUPL_DESC::ModeDesc.Format`、output colorspace 和显示状态；
- VDD：producer metadata、DXGI format 和 VDD HDR state；
- 无法证明 transfer/primaries 时标记 `domain=unknown`，不得按文件格式猜测。

`source_generation` 在显示模式、色彩空间、分辨率、producer 或 capture session 重建时递增，
用于禁止后处理层复用旧资源和旧动态元数据。

### 6.6 输入规范化

TrueHDR 后端只接受一种主机契约：

```text
Format: DXGI_FORMAT_B8G8R8A8_UNORM
Transfer: sRGB / Rec.709 display-referred
Primaries: Rec.709
Alpha: ignored, host writes 1.0
Dimensions: stream active rectangle dimensions
```

新增 `normalize_sdr_cs.hlsl`：

- BGRA8 可在尺寸、旋转和裁剪都一致时直接 `CopyResource`；
- RGBA8、缩放、旋转、裁剪统一由 compute shader 写入私有 BGRA8；
- 不把 `R10G10B10A2_UNORM` 直接解释成 SDR，除非 `captured_frame_desc_t` 同时明确声明
  Rec.709/sRGB；首期按不支持处理；
- 规范化输出必须是 `D3D11_BIND_SHADER_RESOURCE`，并允许后端按 SDK 要求读取。

规范化层输出新的 canonical descriptor；它可以知道后续 filter 的输入要求，但仍不知道
具体捕获后端：

```cpp
gpu_frame_view_t normalize_for_filter(
  const img_d3d_t &captured,
  const filter_input_contract_t &required);
```

---

## 7. 编码前 Filter Graph

### 7.1 公共接口

首版实际新增：

```text
src/platform/windows/pre_encode_filter.h
src/platform/windows/pre_encode_filter.cpp
src/platform/windows/rtx_hdr/backend_abi.h
src/platform/windows/rtx_hdr/backend_loader.h
src/platform/windows/rtx_hdr/backend_loader.cpp
```

建议接口：

```cpp
namespace platf::dxgi {

  enum class filter_status_e { ready, pending, bypass, failed };

  struct gpu_frame_view_t {
    ID3D11Texture2D *texture = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    captured_frame_desc_t semantic;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  struct filter_result_t {
    filter_status_e status = filter_status_e::bypass;
    gpu_frame_view_t frame;
    std::string_view reason;
  };

  class pre_encode_filter_t {
  public:
    virtual ~pre_encode_filter_t() = default;
    virtual bool requires_detached_input() const = 0;
    virtual filter_result_t process(const gpu_frame_view_t &input) = 0;
    virtual void flush() = 0;
    virtual std::string_view backend_name() const = 0;
    virtual bool degraded() const { return false; }
    virtual std::string_view failure_reason() const { return {}; }
  };
}
```

`d3d_base_encode_device::convert()` 不出现 NGX、SDK 参数或 DLL 函数名。它只：

1. 从捕获纹理和 `captured_frame_desc_t` 构造原始 frame view；
2. 比较 `capture_contract` 与实际 frame descriptor，并完成 SDR 输入规范化；
3. 调用 filter；
4. 根据返回 frame descriptor 选择现有 FP16/PQ 转换；
5. 把 filter 输出送入 HDR analyzer。

TrueHDR filter 的输入检查只允许：

```text
semantic.domain   = sdr_rec709
semantic.encoding = unorm8
semantic.borrowed = false
adapter_luid      = backend device adapter LUID
```

它不检查帧来自 WGC、DXGI 还是 VDD，也不接受 capture backend enum。

### 7.2 资源槽

每个编码图像上下文增加：

```cpp
struct synthetic_hdr_image_ctx_t {
  texture2d_t normalized_sdr;
  shader_res_t normalized_sdr_srv;
  texture2d_t hdr_output;
  shader_res_t hdr_output_srv;
  uav_t hdr_output_uav;
};
```

资源按宽、高、格式缓存；分辨率变化或 device removal 时整体销毁重建。

### 7.3 与现有 convert 的接入点

接入位置为：

```text
Acquire encoder mutex
  → initialize_image_context
  → normalize/copy SDR to private texture
  → Release encoder/VDD mutex
  → pre_encode_filter.process()
  → 选择 TrueHDR FP16 或稳定 fallback 输入
  → RGB→PQ/P010
  → HDR analysis
  → encoder
```

这需要把当前 `convert()` 中“释放 encoder mutex”的逻辑提前为可调用的局部函数和
`fail_guard`，确保每个错误分支只释放一次。

### 7.4 输出分析

重构：

```cpp
prepare_hdr_analysis_source(
  bool snapshot_written,
  const gpu_frame_view_t &post_filter_frame);
```

规则：

- TrueHDR active：分析 `hdr_output`；
- TrueHDR pending/bypass：不把原始 SDR 当作真实 HDR 分析；动态场景元数据保持启动基线；
- TrueHDR 已进入稳定 active 状态后才允许 `scene_metadata_active=true`；
- 如果 P010 compute shader 已写 analysis snapshot，继续使用同一次转换产生的 snapshot，
  避免额外整帧分析 pass；
- Dolby Vision/HDR10+/Vivid 共用同一份 post-TrueHDR 统计。

---

## 8. NVIDIA 后端 ABI

### 8.1 文件布局

```text
tools/rtx_hdr_backend/
  CMakeLists.txt
  src/backend.cpp
  tests/backend_smoke.cpp
scripts/build-rtx-hdr-backend.ps1
```

ABI 头由主仓库定义，并由 backend target 直接引用，避免维护两份副本：

```text
src/platform/windows/rtx_hdr/backend_abi.h
```

主程序 loader、fake backend、真实 backend 和 smoke test 都编译同一个头；ABI 版本不匹配时
loader fail-closed。

### 8.2 版本化 C ABI

```c
#define FOUNDATION_TRUEHDR_CALL __cdecl
#define FOUNDATION_TRUEHDR_ABI_VERSION 1u
#define FOUNDATION_TRUEHDR_GET_API_EXPORT "foundation_truehdr_get_api"

typedef enum foundation_truehdr_status_e {
  FOUNDATION_TRUEHDR_STATUS_OK = 0,
  FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT = 1,
  FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED = 2,
  FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE = 3,
  FOUNDATION_TRUEHDR_STATUS_DEVICE_LOST = 4,
  FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR = 5,
} foundation_truehdr_status_e;

typedef struct foundation_truehdr_config_t {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  float contrast;
  float saturation;
  float middle_gray_nits;
  float peak_nits;
} foundation_truehdr_config_t;

typedef struct foundation_truehdr_api_t {
  uint32_t abi_version;
  uint32_t struct_size;
  foundation_truehdr_status_e (FOUNDATION_TRUEHDR_CALL *create)(
    void *d3d11_device,
    const foundation_truehdr_config_t *config,
    void **instance);
  foundation_truehdr_status_e (FOUNDATION_TRUEHDR_CALL *process)(
    void *instance,
    void *d3d11_device_context,
    void *sdr_input_texture,
    void *scrgb_output_texture);
  void (FOUNDATION_TRUEHDR_CALL *flush)(void *instance);
  void (FOUNDATION_TRUEHDR_CALL *destroy)(void *instance);
} foundation_truehdr_api_t;

typedef const foundation_truehdr_api_t *(FOUNDATION_TRUEHDR_CALL *foundation_truehdr_get_api_fn)(
  uint32_t requested_abi_version);
```

约束：

- `foundation_truehdr_get_api()` 是唯一导出，ABI 版本不匹配时返回空指针；
- `create()` 接收 D3D11 device 和包含尺寸、对比度、饱和度、中灰及峰值亮度的配置，
  成功时写出不透明实例；
- `process()` 接收同一 device 的立即 context、只读 RGBA8/BGRA8 SDR 输入及由 Sunshine
  独占的 FP16 scRGB 输出；
- `OK` 表示结果可编码；所有非 `OK` 状态统一映射为 `filter_status_e::failed`，销毁 primary
  并进入同一个 session-stable degraded fallback；host 不按状态码执行不同的恢复策略，但会把
  create/process 阶段及 `INVALID_ARGUMENT`、`UNSUPPORTED`、`RUNTIME_UNAVAILABLE`、
  `DEVICE_LOST` 或 `INTERNAL_ERROR` 类别保存在稳定失败原因中，供运行状态和诊断展示；
- 所有 ABI 入口捕获内部异常并转换为 `INTERNAL_ERROR`；
- `process()` 不分配主机输出纹理；
- 输入只读，输出由 Sunshine 独占；
- 后端不写注册表、不修改 NVIDIA profile、不创建窗口；
- DLL 卸载前必须销毁所有 handle。

### 8.3 DLL 加载安全

正式包默认从 Sunshine 安装目录解析：

```text
<install>/tools/rtx_hdr/foundation_truehdr_backend.dll
```

当前 loader 已实现：

- 路径必须是绝对路径；
- 使用 `LoadLibraryExW` 和受限 DLL 搜索路径，不依赖当前工作目录；
- ABI version 完全匹配；
- 所有必需导出存在；

以下属于正式组件发布门禁，不是当前自定义开发路径 loader 已实现的运行时保证：

- 组件 manifest 中 SHA-256 匹配；
- 正式包中的第一方 DLL 有 Authenticode 签名；
- vendor runtime 只验证固定版本和 hash，不重新签名。

### 8.4 NGX 生命周期

NVIDIA NGX 的全局生命周期需要串行化。首版不引入额外 worker、跨线程 job 生命周期或队列，
而是在 backend DLL 内用进程级 mutex 串行化 init/create/evaluate/release/shutdown：

- 每个 Sunshine filter 实例拥有一个 NGX feature handle；
- 调用发生在现有 encode/convert 路径，但调用前已经把 borrowed VDD 帧复制为私有纹理并释放
  capture mutex；
- 同一进程的多个 TrueHDR 会话暂时串行进入 NGX；
- 首次 evaluate 失败后，该会话销毁外部 backend 并永久降级到内置 GPU fallback，不逐帧重试；
- 只有性能数据证明全局互斥成为瓶颈、且 SDK 明确允许相应并发方式后，才引入 adapter 分片或
  专用 worker。

---

## 9. 状态机与失败策略

### 9.1 会话状态机

```text
disabled
   │ 配置启用 + 客户端 HDR
   ▼
warming_up（filter 已构造，feature 尚未创建）
   │ 首次 process：同步 create + evaluate
   ├── create/process 失败 ──→ degraded（内置兼容回退）
   ▼
active
   ├── 首次 process 失败 ─→ degraded（内置兼容回退）
   ▼
degraded（本会话不可自动恢复）
```

### 9.2 同步执行边界

首版没有 worker、job queue、取消点或 deadline：滤镜构造只加载 DLL 并保存依赖；feature
`create()` 在首帧 `process()` 中按实际输入尺寸同步懒初始化，随后在同一 convert/encode
调用线程同步 evaluate。首帧的 create 与 evaluate 耗时都会计入 pre-encode 延迟，create
失败也只会在首帧被观察到。滤镜对外只返回 `ready` 或 `failed`。
VDD borrowed 帧会先复制到私有纹理并释放 keyed mutex，因此 SDK 调用不会占用捕获资源，
但其 GPU/驱动耗时仍直接计入当前帧的 pre-encode 时间。首次处理失败后立即永久切换到内置
SDR-in-HDR 回退，不逐帧重试。若实测发现 NGX 调用会无界阻塞，再把独立 worker 或进程隔离
作为后续设计引入，届时才定义 pending、取消、超时和 circuit breaker 语义。

### 9.3 降级画面

降级后仍保持 HDR wire contract，使用已有 SDR→PQ shader：

- SDR reference white 使用客户端能力，缺失时 203 nits；
- 不生成伪造的场景动态元数据；
- 静态元数据保持会话启动值；
- Web UI 明确显示“RTX HDR 已降级，流仍为 HDR 容器”；
- 本会话不自动恢复，防止亮度闪烁。

---

## 10. 色彩与元数据契约

### 10.1 输入

- Rec.709 primaries；
- sRGB/BT.1886 近似 display-referred SDR；
- nominal SDR white 由 Windows SDR white 和客户端报告值共同决定；
- 不接受 native HDR、scRGB FP16 或 PQ 输入进入 TrueHDR；
- 如果 source frame 已是 HDR，选择 native HDR 管线而非 TrueHDR。

### 10.2 TrueHDR 输出

- `R16G16B16A16_FLOAT`；
- linear scRGB；
- 1.0 = 80 nits；
- 输出 SRV 直接进入现有 linear FP16 shader/compute shader；
- TrueHDR 参数在会话开始时形成不可变 snapshot；运行中修改下一会话生效。

### 10.3 峰值策略

首期限制 `peak_nits` 为 400–1000：

- 不复制未经本项目验证的 1000→2000 nits 后缩放补偿；
- 配置值即会话固定的 mastering peak（见 §10.4/§19）；客户端报告的峰值只进 target-display 动态元数据，不得改写静态 mastering metadata；
- 客户端未报告时动态 target-display 元数据回退 mastering peak；
- 未来开放 2000 前必须完成模型版本、输出裁剪、middle-gray 和动态元数据一致性测试。

### 10.4 Synthetic HDR 静态元数据（后续统一化目标）

源显示为 SDR，不能把物理显示器 HDR metadata 当作事实来源。首版 VDD 路径继续使用现有的
客户端能力映射和安全默认值，不新增第二套 metadata 实现。后续若要让 VDD、物理显示器和更多
捕获后端共享同一解析器，再引入：

```text
src/hdr/stream_hdr_metadata.h
src/hdr/stream_hdr_metadata.cpp
```

统一解析函数：

```cpp
SS_HDR_METADATA resolve_stream_hdr_metadata(
  const video::config_t &config,
  const platf::display_t &display);
```

来源顺序：

1. synthetic HDR：客户端 target capabilities；
2. native HDR：主机显示器 metadata，再应用客户端 target 限制；
3. 缺失字段：安全默认值。

Synthetic HDR 默认值：

- primaries：BT.2020；
- white point：D65；
- 静态 max display luminance：与传给 TrueHDR 后端的会话 mastering peak 相同；
- 静态 min display luminance：0.0001 nits；
- MaxCLL：初始等于 mastering peak；
- MaxFALL：0，表示帧平均峰值未知；
- HDR10+/HDR Vivid 的 target display peak 独立使用客户端报告峰值；客户端未报告有效值时
  才回退到 mastering peak。不得为了适配客户端而改写合成 HDR 的静态 mastering metadata；
- 动态分析有效后，逐帧动态元数据来自 post-TrueHDR 帧，但控制通道和 encoder 的
  静态 mastering metadata 不在会话中途互相矛盾。

`video.cpp`、native NVENC、AMF 和 `send_hdr_mode()` 必须调用同一解析函数，禁止各自
重新查询显示器。

### 10.5 动态元数据

- HDR10+、Vivid、Dolby Vision 都消费 post-TrueHDR luminance stats；
- 分析采样序列必须带 `source_generation`，TrueHDR 激活前的 SDR 样本不得在激活后复用；
- TrueHDR 状态切换时清空 temporal smoothing、startup gate 和 histogram；
- DV/HDR10+ dual carry 规则沿用现有实现；
- TrueHDR 不改变客户端动态 HDR 协商逻辑。

---

## 11. 配置、应用模型与 Web API

### 11.1 全局配置

在 `config::video_t` 增加两个全局门禁字段：

```cpp
std::string rtx_hdr;               // "off" | "per_app"，默认 off
std::string rtx_hdr_backend_path;  // 版本化 backend DLL 的绝对路径
```

保存键：

```text
rtx_hdr=off|per_app
rtx_hdr_backend_path=C:\...\foundation_truehdr_backend.dll
```

`per_app` 表示允许应用选择，不表示所有 HDR 应用自动开启。避免用户打开一个全局开关
后对桌面和所有 SDR 应用强制 AI 处理。画面参数属于应用 snapshot，不放在全局配置，
这样同一主机上的不同游戏可以使用不同参数，恢复会话时也不会受配置文件热修改影响。

### 11.2 单应用配置

在 `apps.json` 应用节点增加：

```json
{
  "name": "Example Game",
  "rtx-hdr": {
    "mode": "inherit",
    "contrast": 0,
    "saturation": 0,
    "middle-gray": 50,
    "peak-nits": 1000
  }
}
```

`mode`：

- `inherit`：全局 `per_app` 时仍默认关闭，除非应用模板显式推荐；
- `on`：请求 TrueHDR；
- `off`：显式禁止。

为减少歧义，首期实际规则为“只有 `mode=on` 才启用”。`inherit` 保留未来策略扩展，
当前等价于 off。

在 `proc::ctx_t` 保存解析后的可选配置，并增加只读查询：

```cpp
std::optional<rtx_hdr_app_config_t>
proc_t::get_app_rtx_hdr_config(int app_id) const;
```

`nvhttp.cpp` 在显示准备之前，根据 `launch_session->appid` 把 snapshot 写入 launch session。
Resume 使用正在运行应用的相同 snapshot，不重新读取被用户刚修改的文件。

### 11.3 Web UI

Windows 应用编辑器已新增 RTX HDR 卡片：

- `inherit` / `on` / `off` 模式；
- 对比度；
- 饱和度；
- 中灰；
- 峰值亮度；

高级设置页已新增全局门禁与 backend DLL 绝对路径。能力、组件版本和当前活动会话状态
继续通过运行时 API 展示，不把厂商能力判断放进捕获设置。

后续 UI 可以补充：

- 功能可用性；
- 可选组件版本；
- 安装/修复/卸载；
- 当前活动会话状态。

不要在首期实现 live slider update。它会引入跨线程参数一致性和画面跳变，收益低于风险。

### 11.4 状态 API

扩展 `/api/runtime/hdr` 返回：

```json
{
  "rtx_hdr": {
    "component": "ready",
    "backend_version": "1.0.0",
    "runtime_version": "...",
    "adapter": "NVIDIA GeForce RTX ...",
    "sessions": [
      {
        "id": 12,
        "state": "active",
        "capture_format": "B8G8R8A8_UNORM",
        "output_format": "R16G16B16A16_FLOAT",
        "peak_nits": 1000,
        "p50_gpu_ms": 2.1,
        "p95_gpu_ms": 2.8,
        "timeouts": 0,
        "fallback_reason": ""
      }
    ]
  }
}
```

扩展 `video::hdr_pipeline_status_t`：

```cpp
std::string source_hdr_mode;       // sdr/native_hdr/synthetic_hdr
std::string synthetic_hdr_backend; // none/nvidia_truehdr
std::string synthetic_hdr_state;   // disabled/warming_up/active/degraded
std::string synthetic_hdr_failure_reason;
double synthetic_hdr_gpu_ms_p50;
double synthetic_hdr_gpu_ms_p95;
std::uint64_t synthetic_hdr_timeouts;
```

---

## 12. 构建、打包、更新与许可

### 12.1 开发构建

开发机设置：

```text
RTX_VIDEO_SDK_ROOT=<NVIDIA RTX Video SDK>
```

本地构建命令：

```powershell
.\scripts\build-rtx-hdr-backend.ps1 `
  -SdkRoot 'C:\SDKs\NVIDIA-RTX-Video-SDK-1.1.0'
```

`scripts/build-rtx-hdr-backend.ps1`：

1. 查找 VS Build Tools；
2. 用 MSVC x64 编译 backend；
3. 构建独立硬件 smoke test；
4. 复制开发所需的 `nvngx_truehdr.dll`；
5. 输出到 `build/rtx-hdr-backend/Release/`；
6. 不修改系统 PATH 或注册表。

硬件验证命令：

```powershell
.\build\rtx-hdr-backend\Release\foundation_truehdr_backend_smoke.exe `
  .\build\rtx-hdr-backend\Release\foundation_truehdr_backend.dll
```

主 Sunshine 构建不因 SDK 缺失失败；后端是可选组件。

### 12.2 发布模型

沿用可选 DS5 组件的 pinned manifest 思路：

```json
{
  "schema": 1,
  "version": "1.0.0",
  "abi": 1,
  "url": "https://.../foundation-truehdr-1.0.0.zip",
  "sha256": "...",
  "files": {
    "foundation_truehdr_backend.dll": "...",
    "nvngx_truehdr.dll": "..."
  }
}
```

安装目标：

```text
<Sunshine>/tools/rtx_hdr/
```

更新必须是原子目录替换：下载到临时目录、验证全部 hash、验证 ABI、停止新会话、替换、
下次会话加载。不得覆盖正在加载的 DLL。

### 12.3 许可门禁

正式分发前必须完成：

- NVIDIA RTX Video SDK/NGX 再分发条款审查；
- NVIDIA application ID/产品通知要求确认；
- `docs/legal.md` 增加第三方 runtime 声明；
- release package 保留 NVIDIA notice/EULA；
- 第一方 backend 使用项目签名；vendor DLL 不重签；
- CI 禁止把未批准的 SDK 文件提交到 Git 历史。

如果未获得明确再分发许可，功能仍可支持“用户本地安装 SDK runtime”，但正式安装包和
自动下载功能不得上线。

---

## 13. 可观测性

### 13.1 必需日志

每个会话只记录状态变化，不逐帧刷日志：

```text
RTX HDR requested: app=..., client_hdr=true, adapter_luid=...
RTX HDR source policy: display=SDR, capture=BGRA8, wire=HDR10-PQ
RTX HDR backend ready: abi=1, runtime=..., init_ms=...
RTX HDR active: input=BGRA8, output=FP16-scRGB, peak=...nits
RTX HDR degraded: reason=..., consecutive_failures=...
RTX HDR stopped: frames=..., p50=...ms, p95=...ms, timeouts=...
```

### 13.2 性能计数器

- normalize GPU time；
- private handoff copy GPU time 和执行次数；
- TrueHDR GPU/submit wall time；
- RGB→P010 time；
- 总 pre-encode time；
- worker queue wait；
- canceled/stale jobs；
- timeouts；
- device removal；
- session circuit breaker 次数；
- post-TrueHDR MaxCLL/MaxFALL/percentiles。

复用现有 `SUNSHINE_VRAM_TIMING` 基础设施，新增 filter bucket，不另建互不兼容的计时
系统。

关闭 synthetic HDR 的 VDD 会话中，新增计数器必须满足：

```text
private_handoff_count = 0
normalize_dispatch_count = 0
filter_submit_count = 0
filter_queue_wait = 0
```

descriptor 填充和 capability 分支计入现有 CPU submit bucket，不另起 GPU timestamp query，
避免为了证明“无开销”反而改变 fast path。

---

## 14. 测试方案

### 14.1 无 NVIDIA SDK 的单元测试

首版实际新增：

```text
tests/unit/test_frame_contract.cpp
tests/unit/platform/windows/test_truehdr_backend_loader.cpp
tests/unit/platform/windows/test_pre_encode_filter.cpp
tests/tools/fake_truehdr_backend.cpp
```

覆盖：

- 客户端 SDR 时永不启用；
- H.264/8-bit 客户端永不启用；
- per-app on/off/inherit；
- synthetic HDR 策略解析为 `source_display_intent=require_sdr`；
- synthetic HDR 策略解析为 `capture_contract=sdr_rec709/unorm8/private_handoff`；
- WGC 对通用 `unorm8` contract 选择 BGRA8，测试不引用 RTX HDR；
- WGC 对通用 `float16` contract 选择 FP16，测试不引用输出 `dynamicRange`；
- DXGI/WGC/VDD 都能产生完整 `captured_frame_desc_t`；
- `domain=unknown`、adapter LUID 不匹配和 borrowed frame 被 normalization/filter 层拒绝；
- VDD + `require_private_handoff=false` 保持 existing convert，handoff/normalize/filter mock
  的调用次数均为零；
- VDD + `require_private_handoff=true` 恰好执行一次 handoff，在 filter mock 开始前已释放
  borrowed slot；
- native HDR 不进入 TrueHDR；
- SDR 显示器下生成合法 BT.2020/D65 metadata；
- client max nits 限制 peak；
- ABI 不匹配/导出缺失/hash 错误；
- ready→active、首次 primary 失败→degraded 并切换兼容回退；
- 首帧 process 中同步 create/process 失败不会占用 borrowed capture 资源；
- TrueHDR 激活时清空旧 luminance temporal state；
- 多会话 NGX 调用由进程级 mutex 串行化。

### 14.2 后端 contract test

MSVC 后端测试使用生成纹理：

- 纯黑；
- 18% gray；
- 0–255 灰阶；
- RGB/CMY 色块；
- 高饱和渐变；
- 运动边缘；
- 1920×1080、2560×1440、3840×2160。

检查：

- 输出格式始终 FP16；
- 无 NaN/Inf；
- 黑位稳定；
- 灰阶总体单调；
- 输出亮度不超过会话约束和允许容差；
- 输入纹理内容未被修改；
- resize/flush/recreate 无泄漏；
- device removal 返回明确错误。

AI 输出不使用逐像素 golden image；使用统计不变量、感知截图和 SDK/驱动版本化基线。

### 14.3 集成矩阵

| 维度 | 最低覆盖 |
|---|---|
| GPU | RTX 20、30、40、50 各一张 |
| Windows | Windows 10 22H2、Windows 11 当前支持版本 |
| 捕获 | DXGI、WGC display、WGC window、Zako VDD borrowed texture |
| 编码 | HEVC Main10、AV1 10-bit |
| 分辨率/FPS | 1080p120、1440p120、4K60、4K120 |
| 客户端 | HDR10、HDR10+、HDR Vivid、Dolby Vision 8.1、旧 HDR 客户端 |
| 会话 | 单客户端、双客户端、启动/停止循环 100 次 |
| 异常 | alt-tab、显示模式变化、VDD 重建、驱动重启模拟、组件缺失 |

### 14.4 画质验证

- Windows HDR Calibration 图案；
- SDR 灰阶和 near-black ramp；
- 高光裁剪图；
- 游戏 HUD 与字幕；
- 快速场景切换；
- 原始 SDR、普通 SDR→PQ、TrueHDR 三路 A/B；
- post-TrueHDR FP16 readback 保存为 EXR，仅在诊断构建启用；
- P010 readback 校验 PQ code 与 nits；
- HDR10+/DV metadata 与同帧图像 peak 对齐。

### 14.5 性能验收

相对同分辨率、同编码器、关闭 TrueHDR 的基线：

- 关闭 TrueHDR 的 VDD borrowed/direct 路径不得新增 `CopyResource`、compute dispatch、worker
  submit 或一帧队列；CPU submit time p95 的变化必须落在重复测量噪声内；
- 上述零额外 GPU 工作由 PIX/ETW 或 D3D11 timestamp + 计数器共同证明，不能只以 FPS
  不下降作为证据；
- 新增 host latency p95 不超过一个 frame interval；
- 4K60 无持续队列积压；
- 4K120 若同步处理延迟不能满足门槛，则不得列为受支持模式；
- dropped frame rate 增量小于 0.5 个百分点；
- 30 分钟稳定测试无 D3D11 live object 增长；
- 100 次会话启停无 backend handle、线程或 DLL 引用泄漏；
- 双会话时任一会话失败不得阻塞另一会话的非 TrueHDR 编码。

4K120 是否列为正式支持，由实测数据决定，不作为首个可用版本的阻塞条件。

---

## 15. 建议的分阶段 Pull Request

本节是提交和评审拆分建议，不是当前工作树文件清单；实际落地文件以第 16 节和 19.1 节为准。

### PR 1：会话语义拆分与元数据事实源

目标：不接 SDK，先建立正确架构。

改动：

- `video::config_t.synthetic_hdr`；
- `launch_session_t.synthetic_hdr`；
- `source_display_intent`、`capture_contract_t`、`captured_frame_desc_t`；
- source display、capture frame 与 wire output 三者拆分；
- WGC/DXGI/VDD 通用 contract 映射和 actual descriptor；
- display prep 通用 source intent policy；
- `resolve_stream_hdr_metadata()`；
- 单元测试。

验收：模拟 synthetic HDR 时，策略层输出通用 SDR capture contract；显示器保持 SDR、
WGC 因 `unorm8` contract 请求 BGRA8、编码器仍建立 BT.2020/PQ/Main10，且 WGC、DXGI、
VDD 源码不包含 RTX HDR/NVIDIA 判断。

### PR 2：通用 Pre-encode Filter Graph

目标：厂商无关接入点。

改动：

- `pre_encode_filter_t`；
- SDR normalization shader；
- capture contract 与 actual descriptor 校验；
- 私有 input/output slots；
- keyed mutex 提前释放；
- post-filter HDR analysis；
- mock filter 和状态机测试。

验收：mock SDR→FP16 filter 能完整进入 P010、HDR10+/DV 管线，无 NVIDIA 文件。

### PR 3：MSVC TrueHDR Backend 与 Loader

目标：打通官方 SDK。

改动：

- 版本化 C ABI；
- MSVC backend；
- safe DLL loader；
- worker/scheduler；
- backend contract test；
- capability/status API。

验收：开发机可把测试 SDR 纹理转换为有效 FP16 scRGB；缺失后端时主程序正常运行。

### PR 4：应用配置与 Web UI

目标：用户可控且可诊断。

改动：

- apps.json schema；
- 应用编辑器；
- 组件状态；
- runtime HDR 状态；
- 中英文文案，随后补齐全部 locale；
- API/UI 测试。

验收：只有 per-app `on` 启用，修改参数下一会话生效，UI 显示真实状态而非配置推断。

### PR 5：组件发布与硬化

目标：可发布。

改动：

- 构建/打包脚本；
- pinned manifest；
- hash、签名、原子更新；
- 安装/修复/卸载；
- 法律文档；
- CI release contract；
- 长稳和多 GPU 测试。

验收：干净机器可安装、使用、修复和卸载；没有 SDK 的标准包不受影响。

### PR 6：性能与多会话优化

仅在 PR 3–5 数据表明确认瓶颈后实施：

- adapter 分片 scheduler；
- shared texture worker process 隔离；
- 4K120 专项；
- 2000-nit 校准方案；
- live parameter update。

不得提前把这些复杂度带入首个可验证版本。

---

## 16. 文件改动清单

### 已新增（首版实际落地）

```text
docs/rtx_hdr_stream_implementation.md
src/platform/frame_contract.h
src/platform/frame_contract.cpp
src/platform/windows/frame_contract.h
src/platform/windows/frame_contract.cpp
src/platform/windows/pre_encode_filter.h
src/platform/windows/pre_encode_filter.cpp
src/platform/windows/rtx_hdr/backend_abi.h
src/platform/windows/rtx_hdr/backend_loader.h
src/platform/windows/rtx_hdr/backend_loader.cpp
src_assets/windows/assets/shaders/directx/mock_sdr_to_scrgb_cs.hlsl
tools/rtx_hdr_backend/CMakeLists.txt
tools/rtx_hdr_backend/src/backend.cpp
tools/rtx_hdr_backend/tests/backend_smoke.cpp
scripts/build-rtx-hdr-backend.ps1
tests/unit/test_frame_contract.cpp
tests/unit/platform/windows/test_truehdr_backend_loader.cpp
tests/unit/platform/windows/test_pre_encode_filter.cpp
tests/tools/fake_truehdr_backend.cpp
```

### 修改

```text
src/config.h
src/config.cpp
src/process.h
src/process.cpp
src/rtsp.h
src/rtsp.cpp
src/nvhttp.cpp
src/stream.cpp
src/video.h
src/video.cpp
src/video_colorspace.cpp
src/platform/common.h
src/platform/windows/display_wgc.cpp
src/platform/windows/display_vram.cpp
src/platform/windows/display_vdd.cpp
src/platform/windows/display_vdd_vram.cpp
src/display_device/parsed_config.cpp
src/display_device/session.cpp
src/confighttp.cpp
cmake/compile_definitions/windows.cmake
cmake/packaging/windows.cmake
tests/CMakeLists.txt
docs/configuration.md
docs/legal.md
src_assets/common/assets/web/components/AppEditor.vue
src_assets/common/assets/web/configs/tabs/AudioVideo.vue
src_assets/common/assets/web/composables/useConfig.js
src_assets/common/assets/web/public/assets/locale/*.json
```

---

## 17. 发布验收条件

功能只有同时满足以下条件才可从“实验性”转为“稳定”：

1. 无 SDK/无 NVIDIA GPU 环境的标准构建和现有测试完全不回归；
2. 源显示器确实保持 SDR，客户端收到稳定 HDR10-PQ；
3. 捕获后端只依赖 `capture_contract_t`，源码和公开接口不包含 RTX HDR/NVIDIA 分支；
4. 关闭 RTX HDR 的 VDD borrowed/direct 会话不新增 copy、normalize、filter submit 或排队；
5. WGC、DXGI、VDD 三种捕获后端至少各通过一轮 30 分钟稳定测试；
6. TrueHDR 输出被现有 HDR analyzer 分析，动态元数据与画面来自同一帧源；
7. 控制通道、AVFrame、NVENC/AMF 路径使用同一静态 metadata resolver；
8. 后端缺失、ABI 不匹配、feature create/process 失败及 device removal 状态均能安全降级；
9. 不出现逐帧 TrueHDR/fallback 亮度闪烁；
10. 多客户端不会因全局 NGX 锁或挂起导致非 TrueHDR 会话冻结；
11. 安装包、签名、hash、原子更新和卸载通过干净机器测试；
12. NVIDIA 再分发与 application ID 要求已有书面结论；
13. RTX 20/30/40/50 至少各一张卡通过能力和稳定性测试；
14. 4K60 达到性能门槛；4K120 若未达到，UI 和文档明确标记为非保证模式。

---

## 18. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| NGX API 非线程安全 | 多会话冻结/崩溃 | backend 内进程级 mutex 串行化所有 NGX 调用 |
| SDK 调用不可取消 | encoder 线程仍可能被同步 create/process 卡住 | 提前释放 keyed mutex 仅保证捕获资源不被长期占用；无界阻塞需要后续用独立进程隔离 |
| 后处理策略渗入捕获层 | 每加后端/算法都修改 WGC、DXGI、VDD | 只传 `capture_contract_t`，只返回 `captured_frame_desc_t` |
| 源显示意外进入 HDR | 双重转换/颜色错误 | 显示策略关闭 HDR、格式探测拒绝 FP16 输入 |
| SDR 显示无 HDR metadata | 客户端色调映射错误 | 独立 stream metadata resolver |
| TrueHDR 输出和动态元数据不同源 | 场景映射错误 | analyzer 强制消费 post-filter FP16 |
| 峰值高于模型实际输出 | 高光裁剪 | 首期限制 1000 nits，后续以实测开放 |
| 多 GPU adapter 不一致 | OpenSharedResource/NGX 失败 | 按 LUID 建后端实例，capture/encode/backend 必须同 adapter |
| 组件版本漂移 | ABI 崩溃 | versioned ABI、manifest hash、原子更新 |
| 许可不允许自动分发 | 发布阻塞 | 功能与 runtime 解耦，法律门禁前只支持开发安装 |
| AI 输出改变创作意图 | 用户预期偏差 | 按应用 opt-in，UI 明示 AI SDR→HDR，而非原生 HDR |

---

## 19. 实施顺序结论

工程顺序不能从复制 Vibepollo 的 `nv_truehdr.cpp` 开始。正确顺序是：

```text
先拆开 source display / capture frame / wire output
        ↓
建立通用 capture contract 与 actual frame descriptor
        ↓
建立统一 stream metadata resolver
        ↓
建立厂商无关 pre-encode filter graph
        ↓
让 mock FP16 输出接通现有 HDR analyzer/P010/DV
        ↓
最后接入 NVIDIA SDK backend
```

这样即使 NVIDIA SDK 集成或许可暂时受阻，前两个 PR 仍然形成可测试、可复用的主线能力。
捕获层只提供满足契约的帧，厂商专用代码不会扩散到 WGC、DXGI、VDD、RTSP、编码和动态
HDR 模块。

### 19.1 当前实现状态（2026-08-31）

首期代码已接通以下可运行闭环：

- `rtx_hdr=off` 为默认值。此时 VDD borrowed/direct 保持原路径，不创建 private handoff、
  FP16 输出纹理或 pre-encode filter；
- Windows 配置支持 `rtx_hdr=per_app` 与 `rtx_hdr_backend_path=<绝对 DLL 路径>`；旧的
  `true/on/enabled/1` 输入只作为兼容别名归一化为 `per_app`；
- `apps.json` 支持 `rtx-hdr` 节点，应用启动时 snapshot `mode/contrast/saturation/middle-gray/peak-nits`；
  只有全局为 `per_app`、应用为 `on`、客户端请求 HDR 三个条件同时满足时才激活；
- 激活后，显示准备阶段明确关闭源显示/VDD HDR，并禁用该会话的 Vulkan HDR bridge；捕获契约
  固定为 SDR Rec.709 UNORM8；
- borrowed VDD 帧只在持锁期间复制到 Sunshine 私有纹理，随即归还 VDD slot；厂商后处理不会持有
  捕获资源；
- 外部 DLL 使用 `backend_abi.h` 的版本化 C ABI 和安全绝对路径加载；ABI 不匹配不会进入调用；
- 外部后端首个处理错误后立即销毁，并在整个会话内固定降级到 GPU SDR-in-HDR filter，避免逐帧
  在 TrueHDR/fallback 之间切换；
- post-filter FP16 scRGB 继续复用现有 P010/HDR analyzer/编码链路。
- WebUI 高级设置提供全局门禁和 backend 路径；Windows 应用编辑器提供按应用模式与四个参数；
- `/api/runtime/hdr` 返回配置模式、实际 backend、状态和失败原因。

当前配置示例：

```ini
rtx_hdr = per_app
rtx_hdr_backend_path = C:\\Program Files\\Sunshine\\tools\\rtx_hdr\\foundation_truehdr_backend.dll
```

应用配置示例：

```json
"rtx-hdr": {
  "mode": "on",
  "contrast": 0,
  "saturation": 0,
  "middle-gray": 50,
  "peak-nits": 1000
}
```

真实 backend 已使用 NVIDIA RTX Video SDK 1.1.0 构建。其输入为 BGRA/RGBA8 SDR，输出为带
UAV 的 `R16G16B16A16_FLOAT` scRGB；对比度/饱和度从 UI 的 `-100..100` 映射到 NGX 的
`0..200`。SDK 生命周期被限制在独立 MSVC DLL 中，Sunshine 主程序只依赖稳定 C ABI。

### 19.2 对原 pipeline 的耗时影响

关闭 RTX HDR 时没有新增逐帧工作：不创建 filter、不复制 borrowed VDD 纹理、不分配 FP16
handoff，也不进入 NGX。VDD 不开启 RTX HDR 时仍走原来的 borrowed/direct fast path，因此本方案
对原 pipeline 没有新增 GPU pass 或逐帧排队（除会话创建时读取一次配置、一次轻量策略分支外）。
这里的“无新增逐帧工作”是结构保证，不把测量噪声表述成绝对 0.00 ms。

开启 RTX HDR 时必然增加 GPU 工作，顺序为：

```text
SDR capture →（borrowed VDD 时一次 private copy）→ NGX TrueHDR
            → FP16 scRGB → 原有 RGB→P010/HDR analyzer → encoder
```

额外时间来自一次可选的同 GPU copy 和一次 NGX dispatch。private copy 的目的不是画质转换，
而是尽快释放 VDD slot，避免厂商后处理时长反向占用捕获资源。该开销只属于明确 opt-in 的
HDR 会话，不影响 SDR 会话、原生 HDR 会话或未启用 RTX HDR 的 VDD 会话。外部 backend 首次
失败后会话固定降级，不逐帧重试，因此失败场景也不会产生持续抖动。

### 19.3 本机验证记录

本机环境：Windows 11、NVIDIA GeForce RTX 5080、RTX Video SDK 1.1.0。

- 真实 NGX hardware smoke：1920×1080 SDR → FP16 scRGB，RTX 5080 上通过；
- frame contract：10/10 通过，包含“SDR capture + PQ output”的信号解耦回归用例；
- pre-encode filter（WARP、fake backend、故障降级）：5/5 通过；
- backend ABI loader：3/3 通过；
- WebUI：124/124 通过，Vite production build 通过；
- 主程序：无空格 source junction 下 369 步完整编译并链接，`sunshine.exe --version` 正常退出；
- 为兼容本机构建环境，WGC 的 `MinUpdateInterval` 使用编译期能力检测，UPnP 同时兼容
  MiniUPnPc API 17/18，启动期 dark-mode once 改用 Windows `InitOnceExecuteOnce`。

本机升级到 GCC 16 后，仓库全局 `-static` 的 GoogleTest 进程会在所有断言完成后于静态
libstdc++ 清理阶段异常退出；同一批已编译对象改用动态 C++ runtime 后三组测试均以退出码 0
结束。该工具链问题不出现在 MSVC NGX backend，也不出现在完整 `sunshine.exe` 的启动/退出中。

已完成一次真实外网 Moonlight 客户端端到端验收：3168×1440、120 fps、AV1、客户端请求 HDR。
服务端同时观察到：

```text
VDD: BGRA8 SDR, borrowed_texture=true
TrueHDR backend: external_sdr_to_hdr, active
conversion: FP16 scRGB -> P010 direct UAV
encoder: AV1 NVENC, PQ + BT.2020, 10-bit
static metadata: session-stable TrueHDR mastering peak
dynamic metadata: HDR10+ scene analysis active
```

客户端最终显示 `HDR10+`，并确认画面对比度明显提升。这同时验证了两件事：像素确实经过
TrueHDR，而非单纯把 SDR 错标成 HDR；VDD 的 SDR 捕获事实也没有阻止编码输出独立发布 PQ/HDR
信号。

初次联调时，原有客户端亮度适配曾把 1000-nit TrueHDR 处理结果的静态 metadata 改写为客户端
报告的 2000 nit。PR 前审计已收紧该边界：合成 HDR 保留与后端参数相同的会话 mastering peak，
避免像素处理峰值与信号不一致；HDR10+/HDR Vivid 另用客户端峰值作为 target display，确保
例如 1000-nit 母版发往 400-nit 客户端时，动态色调映射目标仍为 400 nits。原生 HDR 继续沿用
现有客户端适配行为，Dolby Vision L6 继续只描述内容母版。

本次联调还暴露并修复了两个闭环问题：

1. fallback shader 资产缺失曾提前终止 filter 创建，使可用的外部 NVIDIA backend 也无法加载；
   现在 primary backend 独立加载，fallback 缺失只降低容灾能力，不再阻断主路径；
2. 旧的 `display.is_hdr()` 门禁让已完成 TrueHDR 的帧仍按 SDR 编码；现在输出色彩空间与 metadata
   统一由 post-filter 输出契约决定。

本次外网会话证明功能链闭环，但没有形成可重复的 RTX HDR on/off 成对延迟样本。因此性能结论
目前限定为代码路径保证：off 不新增 GPU pass；on 增加 private copy（borrowed VDD）与 NGX
dispatch。4K60/4K120 的 p50/p95 数值仍按发布验收条件另做固定场景 A/B 基准，不能用本次主观
验收替代。

---

## 20. 外部技术依据

- NVIDIA RTX Video SDK 支持 SDR→HDR tone mapping，并提供 DX11、DX12、Vulkan、CUDA
  接口：<https://developer.nvidia.com/rtx-video-sdk/getting-started>
- NVIDIA NGX Programming Guide 明确要求调用方负责线程安全：
  <https://docs.nvidia.com/rtx/ngx/programming-guide/>
- Vibepollo 的参考实现证明了 SDR D3D11 纹理→TrueHDR→FP16 scRGB→Sunshine PQ
  管线的可行性，但本方案不沿用其把厂商逻辑直接嵌入 `display_vram` 的结构：
  <https://github.com/Nonary/Vibepollo/tree/master/tools/truehdr_shim>
