# Dolby Vision Profile 8.1 串流实施方案

## 1. 定位

Dolby Vision 8.1 **不是 HDR10+ 的画质升级版**，而是一条并列的终端原生输出通道。

两者的基础画面是同一份 HEVC Main10 / BT.2020 / PQ / 10-bit 视频，差别只在动态元
数据的外壳，以及**最终由谁执行 tone mapping**：

| | HDR10+ | Dolby Vision Profile 8.1 |
|---|---|---|
| 基础视频 | HDR10 PQ / BT.2020 / 10-bit | 同左 |
| 动态元数据 | SMPTE ST 2094-40 | Dolby Vision RPU |
| 元数据载体 | HEVC prefix SEI (T.35) | HEVC UNSPEC 62 NAL |
| 映射执行者 | 客户端 shader，或设备 HDR10+ 引擎 | 设备 Dolby Vision 引擎 |
| HDR10 回退 | 支持 | 支持（BL 兼容性 ID = 1） |

因此本方案的收益不在于"多传了什么画质"，而在于覆盖那些**支持 Dolby Vision 但不
支持 HDR10+ 的终端**（LG、索尼、部分小米/OPPO 生态），并把映射交给终端现成的
Dolby 引擎。

如果客户端最终仍用自研 shader 解析 RPU 再自行映射，这条通道没有意义 —— 继续优化
现有 HDR10+ 链路更有价值。

### 首期范围

```text
Foundation Sunshine / Windows
        ↓  HEVC Main10 · BT.2020 · PQ · HDR10 兼容基础层 + Profile 8.1 RPU
Moonlight V+ / Android
        ↓  video/dolby-vision MediaCodec → Direct Surface
终端 Dolby Vision 引擎映射
```

明确不做：AV1 Dolby Vision、HLG Profile 8.4、Profile 7 双层/FEL、L2/L8 人工调色
Trim、FrameGen/ImageReader/Vulkan 后处理共存、4K120 首发硬指标、非 Android 平台的
"伪原生 Dolby Vision"。

---

## 2. 设计原则

### 2.1 基础格式与动态元数据格式必须拆开

沿用现有 `formats_t` 的思路 —— 基础视频模式（SDR / HDR10-PQ / HLG）与动态元数据
格式（NONE / HDR10_PLUS / VIVID_PQ / VIVID_HLG / DOLBY_VISION_PROFILE_81）正交。

Dolby Vision 8.1 在协议中表示为：

```text
base_hdr_mode      = HDR10_PQ      (dynamicRangeMode 保持 1)
dynamic_hdr_format = DOLBY_VISION_PROFILE_81
codec              = HEVC
```

不新增会破坏旧协议的 `hdrMode = 4`；旧版 Sunshine 遇到未知值不能当成 HLG 或 SDR。

### 2.2 一个会话只选一种动态 HDR 格式

连接建立时一次性决定。运行中不允许在同一解码器会话里动态切换。切换与降级都走：

```text
停止视频会话 → 重新协商 → 重建解码器 → 请求 IDR → 恢复播放
```

已实现说明：DV 激活时 HDR10+ SEI **仍然并行发送**（HDR Dual Carry 实践，
ST 2094-40 与 DV RPU 共存于一条码流是既有生态做法）。DV 解码器忽略 SEI，
HDR10+ 设备忽略 RPU，互不干扰；降级路径也因此天然存在。"只选一种"约束的
实质是不中途切换，而非互斥传输。

### 2.3 Dolby Vision 必须走 Direct Surface

原生 DV 的价值在于让终端 Dolby 引擎接管输出，因此
`MediaCodec → renderTarget.surface` 是唯一合法路径。客户端的 `framegenSurface` /
ImageReader 中转分支在 DV 模式下必须硬性禁止。

---

## 3. 主机端改造

现有基础设施已经足够，**不需要重写 HDR 捕获与分析管线**：

- D3D11 GPU 亮度分析与归约 shader（`display_vram.cpp`）；
- PQ / P010 转换过程中同步分析；
- 每帧峰值、平均值、九档百分位（`platf::hdr_frame_luminance_stats_t`）；
- HDR10+ ST 2094-40 与 CUVA 序列化（`video_hdr_metadata.h`）；
- 编码器 side data 与编码后码流注入（`video_hdr_bitstream.h`，AMF 路径已在用）。

只需增加一个新的输出后端。

### 3.1 RPU 生成模块

新增 `src/video_dolby_vision.h` / `.cpp`，命名空间 `video::dolby_vision`。

**后端选择：原生 C++ 写入器，不链接 libdovi。** 理由：

1. 本仓库刻意把 Rust/cargo 限制在可选的 Tauri GUI 上（`cmake/packaging/FetchGUI.cmake`
   的存在就是为了从主构建移除 Rust 依赖），链接 libdovi 会让它变成强制依赖；
2. 仓库已有手写 CUVA T.35 序列化器与 `detail::bit_writer_t` 的先例；
3. Profile 8.1 的 no-op 映射 RPU 是固定模板，每帧只改 L1 的 3×12 bit 和
   `scene_refresh_flag`，再重算尾部 CRC —— 稳态零堆分配比 libdovi C API
   （每帧构造 Rust 对象、返回 owned `Vec`）更容易达成。

libdovi / `dovi_tool` 仍用于**开发期与 CI 的交叉验证**（离线 `dovi_tool info`
解析 golden RPU），不进入实时路径。实时路径禁止：写临时 JSON、启动子进程、磁盘 IO。

#### 接口

```cpp
struct session_config_t {
  uint16_t source_mastering_peak_nits = 1000;
  uint16_t mastering_min_nits_x10000 = 1;   // L6 min，单位 1/10000 nit
  uint16_t max_cll_nits = 0;
  uint16_t max_fall_nits = 0;
  uint16_t active_area_left = 0, right = 0, top = 0, bottom = 0;  // L5
};

struct frame_metadata_t {
  uint16_t min_pq = 0;   // 12-bit PQ 码值
  uint16_t avg_pq = 0;
  uint16_t max_pq = 0;
  bool scene_refresh = false;
};

class rpu_generator_t {
public:
  bool configure(const session_config_t &config);   // 会话启动时构建双模板
  /// 返回完整的 UNSPEC 62 NAL（含 0x7C 0x01 头与防竞争字节），
  /// 指向内部缓冲，有效期至下一次 generate() 或 reset()。
  std::span<const uint8_t> generate(const frame_metadata_t &metadata);
  void reset();
};

/// L1 元数据推导：avg 取 stats.avg_maxrgb_pq（PQ 域均值），
/// max 取 percentile_99（离群点防护）。有扩展近黑统计时，近黑覆盖率达到 1%
/// 才把 min 报告为零，否则取 percentile_1_pq；旧分析结果回退到 percentile_10_pq。
/// 所有值随后钳位。
std::optional<frame_metadata_t>
frame_metadata_from_stats(const platf::hdr_frame_luminance_stats_t &stats);

/// 在场景边界之间统一平滑 min/avg/max；切场时先 reset()，避免旧场景拖尾。
class level1_temporal_filter_t { ... };

/// frame_id 绑定的固定容量在途 RPU 队列：编码输出按 frame_index 取回对应 RPU，
/// 超过最大在途帧数返回失败 —— 调用方应停止 DV 而不是错位附接。
class staged_rpu_queue_t { ... };
```

#### RPU 最小集合

首期只生成 L1（每帧动态 min/avg/max PQ）、L5（活动画面区域，默认全 0）、L6（源母版
亮度 + MaxCLL/MaxFALL），以及必要的版本与兼容性数据。

不生成：L2 / L3 / L8 / 人工 Trim / Enhancement Layer / 非零 reshaping residual。

固定参数（对齐 `dovi_tool` 的 `p8_default()` 与 `Profile81::dm_data()`）：

```text
rpu_type = 2, rpu_format = 18
vdr_rpu_profile = 1, vdr_rpu_level = 0
coefficient_log2_denom = 23, vdr_rpu_normalized_idc = 1
bl_bit_depth_minus8 = 2, el_bit_depth_minus8 = 2, vdr_bit_depth_minus8 = 4
disable_residual_flag = 1        → EL present = 0
vdr_dm_metadata_present_flag = 1 → RPU present = 1
mapping: 每分量 1 段多项式，poly_coef_int = {0, 1}, poly_coef = {0, 0}
         → 恒等映射（no-op），移除 RPU 后基础层仍是正常 HDR10
signal_eotf = 65535, signal_bit_depth = 12, signal_full_range_flag = 1
source_diagonal = 42
```

CRC 为 CRC-32/MPEG-2（poly 0x04C11DB7, init 0xFFFFFFFF, 无反射, 无异或输出），
覆盖 `0x19` 前缀之后到 CRC 之前的全部字节，随后写入 `0x80` 终止字节。

**为什么模板几乎可以只打 L1 的补丁：** L1 在扩展块列表中长度固定（5 字节 / 36 有效 bit
+ 4 bit 对齐），块按 level 排序，且写入 L1 之前的所有字段长度都与 L1 的取值无关。
因此 L1 的 36 bit 在模板中的**比特偏移是常量**，补丁后只需重算 CRC。

唯一的例外是 `scene_refresh_flag`：它是 `ue(v)`，0 编码为 1 bit 而 1 编码为 3 bit。
`num_ext_blocks` 之后的字节对齐填充恰好吸收这 2 bit 差，所以两个变体总长相同、
L1 偏移也相同 —— 但这只是巧合，不应成为依赖。`configure()` 仍构建**两个模板**
（scene_refresh = 0 与 1），各自预计算 L1 比特偏移。

### 3.2 L1 元数据推导

现有统计不能机械复制成 DV 的语义：

- **`min_pq` 不能用绝对最小像素。** 游戏中一个黑色 UI 像素、黑边或透明合成区域就能
  把整帧最小值钉死在零。分析器同时给出 PQ 第 1 百分位和首个 PQ 直方图 bin 的覆盖率；
  覆盖率达到 1% 才报告零，否则采用第 1 百分位。旧分析结果没有扩展统计时回退到 P10。
- **`avg_pq` 必须是 PQ 域平均，不是 `average_maxrgb`。** PQ 是凹函数，
  `PQ(mean(nits)) ≥ mean(PQ(nits))`，暗场带高光时差距是整个动态范围的大部分。
  现有 `stats.avg_maxrgb_pq` 正是逐像素累加的 PQ 域平均，直接用它。
- **`max_pq` 沿用现有的离群点防护。** 用第 99 百分位（`stats.percentile_99`）而不是
  真实峰值 —— Windows 合成器 scRGB 表面的镜面过冲无上限，少量像素就能把整帧钉到
  10000 nits。真实峰值保留供诊断。

`dovi_tool` 的 `clamp_values_int()` 定义了合法域，实现必须照做：

```text
min_pq ∈ [0, 12]
max_pq ∈ [2081, 4095]
avg_pq ∈ [819 (CM v2.9) 或 1229 (CM v4.0), max_pq - 1]
```

### 3.3 不把客户端峰值亮度写进 L1

现有 HDR10+ 会携带 `targeted_system_display_maximum_luminance`，那是 ST 2094-40
模型的一部分。DV 路径不要照搬 —— RPU 应描述内容本身，终端 Dolby 引擎已经知道本机
显示能力。

```text
source_mastering_peak  →  属于内容 / 主机端元数据（写入 L6）
client_display_peak    →  只用于能力选择与诊断
```

### 3.4 HEVC RPU 注入

在 `video_hdr_bitstream` 中增加 UNSPEC 62 的注入路径。与现有 T.35 prefix SEI 注入
的关键差别：**RPU 放在该画面的最后一个 NAL 位置**（`dovi_tool` 专门调整过 RPU 在
AU 中的位置以提高播放器与设备兼容性），而 T.35 SEI 放在第一个 VCL NAL 之前。

处理规则：

1. 解析完整 Access Unit；
2. 移除该 AU 中已有的 UNSPEC 62 RPU（幂等性）；
3. 保留 VPS / SPS / PPS / AUD / prefix SEI / VCL；
4. 每个 coded picture 只插入一个 RPU；一帧多 slice 时也只插一次；
5. RPU 与生成它的同一 `frame_id` 绑定；
6. 保持 Annex-B 起始码风格；
7. 对异常 NAL、截断数据、超大 RPU 返回失败而不是输出破损码流。

### 3.5 RPU 与编码帧的严格绑定

必须建立显式映射，**不能依赖「第 N 次捕获对应第 N 次编码回调」**：

```text
捕获 frame_id → GPU analysis frame_id → 编码器 opaque/timestamp
              → 编码输出 packet frame_id → 匹配 RPU → 注入对应 AU
```

编码器会丢帧、延迟输出、在重建 IDR 时改变输出节奏、因码率重配置刷新内部队列、
一帧输出多个 packet。

现成的机制可以复用：AMF 路径的 `staged_metadata_units`（`frame_index` 键控的
map + `MAX_TRACKED_FRAMES` 上限）已经是这个模式；NVENC 用
`pic_params.inputTimeStamp = frame_index` / `lock_bitstream.outputTimeStamp` 往返
传递帧号。

超过最大在途帧数仍未匹配，应记录错误并停止 Dolby Vision，**而不是把旧 RPU 附到新
画面上** —— 错一帧比没有动态元数据更糟，爆炸、高光闪烁、快速切场时会出现明显的
亮度泵动。

### 3.6 编码会话接入（已实现）

`rpu_injector_t`（`video_dolby_vision.h`）是会话级 DV 状态机：

```text
make_nvenc/amf_encode_session
    └─ 协商为 DV 且 分析器可用 且 最终色彩空间为 PQ 时
       从主机显示的【原始】mastering 元数据（未经客户端峰值调整）配置 L6
encode_frame(frame_index)            提交侧
    └─ stage(frame_index, hdr_luminance_stats)
       统计缺失时复用上次有效值；首帧前无统计则不发 RPU（与 HDR10+ 冷启动一致）
encode_nvenc/amf 输出侧
    └─ inject(encoded_frame.frame_index, data)
       按编码器自己的输出帧号取回 RPU，追加为 AU 最后一个 NAL
在途队列溢出
    └─ 停止本会话 DV（清空队列，宁可无 RPU 也不错位）
```

范围限定：**仅 NVENC 与 AMF 原生路径**。avcodec 路径（软件/QSV）的 AVPacket
无法原地扩容，注入不可行 —— 协商为 DV 却落到 avcodec 编码器时记录警告并继续
发 HDR10（基础层本就兼容）。分析器不可用或最终色彩空间非 PQ 时同样跳过。

---

## 4. 协议协商

### 4.1 客户端能力

在现有扩展协议中使用位掩码，避免为每种格式增加独立字段：

```text
dynamic_hdr_caps:
  bit 0 = HDR10+
  bit 1 = HDR Vivid PQ
  bit 2 = HDR Vivid HLG
  bit 3 = Dolby Vision Profile 8.1

另带：
  dolby_vision_native_surface   （客户端确认走 Direct Surface）
```

（设计稿中的 `dolby_vision_max_level` 已删除：主机从不按 level 门控，
客户端在真实分辨率/帧率上的 isFormatSupported 探测是更强的检查，
且设备 level 上报不可靠。）

**已落地的线格式**（`src/hdr/dynamic_hdr_selection.h`，RTSP ANNOUNCE SDP 携带）：

```text
客户端 → 主机（ANNOUNCE SDP a= 行，缺省均为传统客户端语义）：
  x-ss-video[0].dynamicHdrCaps             位掩码，见上；主机对未知位 mask-off
  x-ss-video[0].dolbyVisionDirectSurface   0/1
  x-ss-video[0].dynamicHdrPreference       0 自动 / 1 DV / 2 HDR10+ / 3 仅 HDR10

主机 → 客户端（ANNOUNCE 200 响应头）：
  X-SS-Dynamic-HDR:            <0-4>  选择的格式（0 none, 1 HDR10+,
                                        2 vivid_pq, 3 vivid_hlg,
                                        4 dolby_vision_profile_81）
  X-SS-Dynamic-HDR-Fallback:   <枚举名>  仅当客户端具备/请求了 DV 却未获选时出现

主机侧无开关：协商无条件参与（客户端上报能力即协商）。开关在 Sony 电视
端到端点亮验证后移除 —— 安全边界由协商模型本身提供：客户端不报 DV 位
就永远不会协商出 DV，任何门失败直落 HDR10。
```

兼容规则：**未上报能力位的旧客户端保持现状** —— HDR10+ 仍无条件发送（历版
Sunshine 的行为）；只有显式上报能力位的客户端才会被协商降级（例如上报掩码
不含 HDR10+ 位 → 纯 HDR10）。畸形参数整体回退到传统语义，单个坏字段不会把
客户端悄悄降级。

### 4.2 主机选择逻辑

```cpp
if (client.dv81 && codec == HEVC && colorspace == BT2020_PQ && bit_depth == 10 &&
    client.direct_surface && !client.frame_generation &&
    host.rpu_generator_available && host.hevc_packet_injection_available) {
  selected = DOLBY_VISION_PROFILE_81;
} else if (client.hdr10plus && host.hdr10plus) {
  selected = HDR10_PLUS;
} else {
  selected = NONE;  // 普通 HDR10
}
```

优先级：用户选「自动」→ DV 8.1 → HDR10+ → HDR10；用户明确选 HDR10+ → HDR10+ →
HDR10；用户明确选 Dolby Vision → **DV 8.1 → HDR10（直落）** —— 客户端的 DV
请求只携带 DV 能力位，主机任一门失败即裁决为纯 HDR10，不经过 HDR10+ 中转。

服务器必须把最终结果回传：`selected_dynamic_hdr_format`、
`selected_dolby_vision_profile`、`selected_dolby_vision_level`、`fallback_reason`。

---

## 5. 客户端改造（Moonlight V+，不在本仓库）

### 5.1 能力探测

**显示能力**：检查当前实际输出 Display 而不是「设备是否支持过」DV —— 需结合当前
活动显示模式支持的 HDR 类型，避免手机内屏支持、外接显示器不支持却错误开启。

**解码器能力**：查找 `MediaFormat.MIMETYPE_VIDEO_DOLBY_VISION`（`video/dolby-vision`）。
Profile 8 对应 `MediaCodecInfo.CodecProfileLevel.DolbyVisionProfileDvheSt` ——
**不要误用 `DolbyVisionProfileDvheDtb`，那是 Profile 7**。

沿用现有 `HdrDecoderProfileSelector` 的严格探测方式（分辨率、帧率、Profile、色彩
空间联合探测）。不能因为普通 HEVC 解码器支持 4K120 就推断 DV 解码器也支持。

### 5.2 Dolby Vision 配置记录是首个技术门槛

不要假设仅设置 `KEY_PROFILE = DolbyVisionProfileDvheSt` 所有厂商解码器都会自动
识别为 Profile 8.1。配置记录至少要能表达：

```text
dv_profile = 8, dv_level = 与分辨率/帧率匹配
rpu_present_flag = 1, el_present_flag = 0, bl_present_flag = 1
dv_bl_signal_compatibility_id = 1
```

第一阶段必须同时验证三种初始化方式：
1. `video/dolby-vision` + Profile/Level，直接输入带 RPU 的 Annex-B；
2. Profile/Level + 显式 Dolby Vision configuration record；
3. VPS/SPS/PPS CSD 与 configuration record 的厂商要求组合。

**这项验证通过之前，不要开始大规模改 Sunshine 的动态 RPU 生成。**

### 5.3 强制关闭冲突功能

DV 8.1 激活时禁用：FrameGen（`framegenSurface` 必须为 null）、ImageReader 中转、
客户端 HDR tone mapping、Vulkan/GL 色彩重映射、客户端锐化后处理、解码后截图分析。

允许：PTS 精确同步、MediaCodec timed release、Surface 帧率匹配、系统合成的控制
菜单、独立性能 Overlay、输入与触觉。Overlay 必须作为单独 Surface/UI Layer 叠加，
不能把 UI 烧进视频画面后重新输出。

### 5.4 运行时状态与降级

```text
CAPABILITY_CONFIRMED → CODEC_CONFIGURING → WAITING_FIRST_FRAME → DOLBY_VISION_ACTIVE
```

故障（configure/start 失败、输入 RPU 后 CodecException、首帧超时、输出格式异常、
连续解码失败）→ 记录设备故障签名 → 结束视频会话 → 以 HDR10+ 重连。

故障签名：`Build.FINGERPRINT`、codecName、displayId、displayModeId、resolution、
fps、profile、level、errorCode。缓存后本应用生命周期内不再尝试 DV，避免无限重连。

### 5.5 诊断

DV **不应假设存在** HDR10+ 那样的 `KEY_HDR10_PLUS_INFO` 逐帧输出查询接口。改为
记录：请求/协商格式、Display 支持性、Codec 名称、Profile、Level、配置记录是否传入、
Direct Surface 与否、首帧成功与否、输出 Format 摘要、Codec 错误码、降级原因枚举。

---

## 6. 实施阶段与 PR 拆分

### Phase 0：设备端可行性验证（首个 Go/No-Go）

制作一条已知正确的 Profile 8.1 测试码流（HDR10 HEVC Main10 + 固定但合法的 RPU），
在 Moonlight V+ 的同一套 MediaCodec 输入代码中本地回放验证，不接网络：
`video/dolby-vision` 能配置、`DvheSt` 被接受、RPU NAL 能输入、首帧正常、显示设备
实际进入 DV、Direct Surface 正常、1080p60 与 4K60 分别验证、外接显示器重新验证、
去掉 RPU 后仍可作为 HDR10 解码。

### Phase 1：Sunshine RPU 传输证明（主机端已完成）

先用固定的合法 RPU，不追求动态画质。新增 RPU 写入器、HEVC 注入器、feature flag、
golden bitstream 测试。验收：每个 AU 恰好一个 RPU、`dovi_tool info` 可解析全部
RPU、去除 RPU 后基础层仍是 HDR10、网络分片/重传/IDR 后仍可解码、不增加一帧缓冲。

已落地：原生 RPU 写入器（dovi_tool 交叉验证）、HEVC 注入器与 golden tests、
NVENC/AMF 编码会话接入（§3.6）。**网络 + 客户端侧端到端已验证**（2026-08-25
OPPO 真机解码链路 + Sony 电视 Dolby Vision 点亮）；主机侧灰度开关在验证通过
后移除，协商无条件参与。

### Phase 2：实时 L1 元数据（主机端已随编码接入落地）

接入 GPU 分析数据，加入暗部稳健统计、场景切换检测、峰值离群抑制、首帧预热、
统计缺失时的保守 RPU、RPU/frame_id 严格匹配、零分配优化。

已落地：avg/max/min 推导与钳位（§3.2）、统计缺失复用上次有效值、首帧预热跳过、
frame_index 严格绑定、队列溢出即停、稳态零分配。L1 min/avg/max 使用专用时域滤波器；
`scene_refresh` 由独立 GPU 样本的 PQ 均值、P10/P90 与 HDR10+ 分位分布共同判定，
切场时先清空滤波历史，重复使用同一分析样本不会重复刷新。
待实机调优：按游戏类型校准切场阈值与近黑覆盖率阈值。

### Phase 3：正式协议协商与降级（协商层已落地）

客户端 DV capability、主机 dynamic HDR selection、回传最终选择、自动降级 HDR10+、
设备失败缓存、性能 Overlay 状态、日志与诊断导出。

已落地：§4.1 线格式与选择逻辑。待客户端侧：设备失败缓存与降级重连（§5.4）、
诊断界面（§5.5）。

### Phase 4：质量调优与设备矩阵

在**同一设备**上比较 DV 8.1 / HDR10+ / HDR10。不要拿不同设备的屏幕观感直接对比
格式优劣。

### 推荐 PR 顺序

1. 协议：动态 HDR 能力与选择结果
2. Sunshine：RPU 写入器（本仓库，Phase 1 的一半）
3. Sunshine：HEVC RPU 注入器与 golden tests
4. V+：Dolby Vision capability probe
5. V+：`video/dolby-vision` Direct Surface 解码
6. 端到端固定 RPU 实验模式
7. Sunshine：实时 L1 RPU
8. 运行时降级、失败缓存与诊断
9. 设备兼容与画质调优
10. 授权、命名与发布开关

---

## 7. 测试与验收

### 7.1 码流正确性

Profile = 8；BL compatibility ID = 1；RPU present = 1；EL present = 0；
BL present = 1；每画面一个 RPU；无重复 NAL 62；RPU 可被 libdovi/`dovi_tool` 完整
解析；重复执行 injector 不改变结果；删除所有 RPU 后仍可按 HDR10 解码；VPS/SPS/PPS
重发和 IDR 后仍正常；单帧多 slice 不重复注入；编码器丢帧时不发生 RPU 错位。

### 7.2 画质测试图

近黑 0～5 nit 阶梯；1%/5%/10%/25% 白窗口；100/400/600/1000/2000/4000 nit 高光；
0～10000 nit PQ ramp；极少量超亮像素；大面积高亮场景；夜景与霓虹；快速明暗切场；
静态 HUD 加动态游戏画面；Windows SDR UI 叠加 HDR 游戏；黑边、字幕、透明窗口。

观察：黑位是否抬升、暗部是否被压死、高光是否提前 roll-off、场景切换是否闪烁、
HUD 亮度是否呼吸、整体是否无故变暗、长时间静态画面是否稳定。

### 7.3 性能内部门槛

工程验收目标（非 Dolby 标准要求）：

```text
新增完整帧 GPU pass            0
新增解码后图像 copy            0
新增视频帧队列                 0
稳态每帧堆分配                 0
RPU 生成 + 注入 P95            < 0.2 ms
RPU/frame 错配计数             0
相对 HDR10 的主机端延迟增量    ≤ 0.2 ms
```

### 7.4 设备兼容矩阵

两类不同 SoC 的 DV 手机；一类 Android TV / Google TV；手机内屏；USB-C/HDMI 外接
显示器；支持 DV 但不支持 HDR10+ 的设备；同时支持两者的设备；只支持 HDR10 的设备；
错误声明 DV Profile 的异常设备；1080p60 / 1440p60 / 4K60。120Hz 仅作能力探测，
不作首期承诺。

---

## 8. 风险排序

1. **Android MediaCodec 初始化（最高）** —— 设备显示 DV、CodecList 中出现
   `video/dolby-vision`，仍不代表原始 Annex-B 串流一定能按 Profile 8.1 正确启动。
   必须先验证 configuration record、CSD 与 Surface 路径。
2. **现有统计与 DV L1 语义不完全一致** —— HDR10+ 的 `average_maxrgb`、百分位峰值
   不能机械复制成 `avg_pq` / `max_pq`。需 golden vector、离线工具、实机画面三方
   交叉验证。
3. **RPU 与编码画面错位** —— 错一帧比没有动态元数据更糟。
4. **后处理功能冲突** —— FrameGen、ImageReader、客户端 tone mapping 与"终端原生
   DV"在架构上冲突。首期必须互斥，而不是试图兼容。
5. **技术可用 ≠ 官方认证** —— 能生成合法 RPU 不等同于 Dolby 产品授权或认证。
   公开使用 Dolby Vision 名称、商标或宣称正式支持前，仍应完成 Dolby 的产品授权、
   技术交付与测试审批流程。

---

## 9. 首发规格

```text
平台：Windows Sunshine → Android Moonlight V+
编码：HEVC Main10 / BT.2020 / PQ / 4:2:0 10-bit
Dolby：Profile 8.1，单层 HDR10 兼容 BL，动态 L1，静态 L5/L6，无 EL，无人工 Trim
显示：MediaCodec video/dolby-vision，Direct Surface
分辨率：1080p60 / 1440p60 / 4K60
降级：Dolby Vision 8.1 → HDR10（直落，客户端 DV 请求只报 DV 位）
```

产品设置（**客户端 UI，位于 Moonlight V+**。格式选择是设备相关决策：同一
主机服务多个客户端，各自能力不同，客户端按设备上报偏好，
`dynamicHdrPreference` 即此 UI 的线格式）：

```text
动态 HDR
  ● 自动               优先使用设备原生动态 HDR
  ○ Dolby Vision 8.1   使用设备原生 Dolby Vision 映射（实验性）
                       与画面补帧、客户端后处理不兼容
  ○ HDR10+             使用 HDR10+ 动态元数据
  ○ HDR10              使用静态 HDR
```

主机侧 UI 决策：**不做格式选择界面**。曾在验证期存在的 `dolby_vision`
灰度开关已在 Sony 电视点亮后移除 —— 安全边界由协商模型本身提供（客户端
不报 DV 位就永远不会协商出 DV）；降级原因、失败缓存、兼容性提示全部由
客户端展示（§5.4/§5.5）。

**最正确的实施顺序不是先把动态 RPU 写进 Sunshine**，而是先用一条确定正确的
Profile 8.1 测试码流打通 Moonlight V+ 的 `video/dolby-vision → Direct Surface`。
该门槛通过后，再复用现有 HDR10+ GPU 分析与 HEVC 注入架构接入实时 L1 RPU。

本仓库能独立完成且是所有后续 PR 前置的部分，就是第 3 节的主机端基础层 —— 那也是
本轮实施的范围。
