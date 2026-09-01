# Dolby Vision Profile 8.4 串流实施方案

## 1. 定位

DV Profile 8.4 与已落地的 8.1 通道（docs/dolby_vision_profile81.md）结构相同：单层 BL+RPU，
UNSPEC 62 NAL 注入 HEVC AU，客户端经 `video/dolby-vision` MediaCodec 直渲染。
唯一区别是基层 EOTF：

| | 8.1 | 8.4 |
|---|---|---|
| 基层 | PQ (ST 2084) | HLG (ARIB STD-B67) |
| 客户端 dynamicRangeMode | 1 | 2 |
| 目标设备 | 主流 DV 设备 | 不接受 PQ-BL RPU、仅认 HLG-BL 的设备（部分 LG 等） |

复用资产：`video_dolby_vision` RPU 写入器、`staged_rpu_queue_t`、`video_hdr_bitstream`
strip/inject、`dynamic_hdr_selection` 协商框架、NVENC/AMF 注入链路、HLG 编码管线
（bt2020hlg P010 已存在）。**不新增编码器代码。**

优先级规则（已确认）：客户端同时报 8.1 与 8.4 时**只协商 8.1**；仅当客户端只报 8.4
（且请求 HLG）时才协商 8.4。

## 2. 与 RTX HDR（PR #1018）的互斥

RTX HDR 管线固定 PQ 编码输出：TrueHDR 滤镜输出 FP16 linear scRGB，由后续
RGB→PQ/P010 编码阶段产生 PQ 码流（HLG 会话不激活滤镜，rtsp.cpp 守卫
`dynamicRange == 1`）。因此：

- app 开启 `rtx-hdr` 时，8.4 **不参与协商**（8.1 不受影响，其会话本就锁 PQ）。
- 实现位置：`select_dynamic_hdr` 的 host gate `synthetic_hdr_enabled` —— 跟随
  **应用特性**（`session.synthetic_hdr.enabled`）而非会话 filter 状态，因为
  HLG 请求下 filter 恒为关闭，用 filter 状态做 gate 会让 RTX HDR 应用在 HLG
  路径漏排除。gate 可在 ANNOUNCE 响应头里给客户端明确 fallback 原因
  （`colorspace_unsupported`）。
- 术语链（`rtx_hdr` / `synthetic_hdr` / `pre_encode_filter` 三层各自的消费
  边界）见 rtx_hdr_stream_implementation.md §2.7。

## 3. 主机端改动

### 3.1 协商层（src/hdr/dynamic_hdr_selection.h/.cpp）

- 新能力位：`DYNAMIC_HDR_CAPS_DOLBY_VISION_84 = 1u << 4`（wire 稳定，旧客户端不会上报）。
- 新格式枚举：`dynamic_hdr_format_e::dolby_vision_profile_84 = 5`（X-SS-Dynamic-HDR wire 值 5，
  不重排现有 0-4）；`to_string` 加 `"dolby_vision_profile_84"`。
- `dynamic_hdr_fallback_e` 不新增值：`colorspace_unsupported` 语义扩展为
  "要求 PQ 但会话为 HLG / 要求 HLG 但会话为 PQ"。
- 选择规则（preference 为 automatic 或 dolby_vision 时，按序判定）：
  1. HEVC 门失败 → codec_unsupported（不变）
  2. `dynamic_range_mode == 1` 且 caps 有 8.1 → **8.1**（现状优先级不变）
  3. `dynamic_range_mode == 2` 且 caps 有 8.4 且无 8.1 → **8.4**
     （caps 同时有 8.1/8.4 且 HLG：属客户端矛盾上报，按 colorspace_unsupported
     落回 —— 8.1 报位意味着客户端应预期 PQ，与 HLG 请求冲突；客户端应自己在
     请求 HLG 时不报 8.1）
  4. RTX HDR per-app 激活 → 8.4 门失败，reason `colorspace_unsupported`
  5. direct surface 门不变
- `dynamic_hdr_selection_t::dolby_vision_active()` 改为 8.1 或 8.4。
- 已知位掩码 `known_bits` 加入 1u<<4。

### 3.2 编码会话（video.cpp / video_dolby_vision）

- 注入门控现状：`协商为 DV && 分析器可用 && PQ`。改为按协商格式分派：
  - 8.1 → colorspace 必须 PQ（现状）
  - 8.4 → colorspace 必须 HLG（dynamicRange=2 的 bt2020hlg P010）
- RPU 写入器零改动：L1（场景光域 min/mid/max）、L5、L6 与基层 EOTF 无关；
  主机显示原始 mastering 元数据配 L6 的路径直接复用。
- 注入位置不变：编码输出侧按 encoder frame_index 注入 UNSPEC 62。
- 会话日志：`NVENC: Dolby Vision Profile 8.4 active (HLG base layer, mastering peak N nits)`。

### 3.3 信号

8.4 会话 colorspace = bt2020hlg、10-bit，走现有 HLG 编码路径；
HDR10+/Vivid SEI：8.4 时 HDR10+ 需 PQ 不再并行发送（Dual Carry 决策仅适用 8.1/PQ），
Vivid HLG 可保留（vivid_hlg 枚举已存在，若分析器可用）。

## 4. 客户端侧（moonlight-common-c fork + moonlight-vplus）

- common-c（mic 分支续）：能力位常量 `DYNAMIC_HDR_CAPS_DOLBY_VISION_84 = 1<<4` 透传；
  `LiGetNegotiatedDynamicHdrFormat()` 无需感知语义（int 直传）。
- vplus：
  - 能力探测：在现有 DvheSt 严格探测外，追加 HLG-BL 判定 —— 设备 DV 解码器接受
    dvhe.08.04（MediaCodec `video/dolby-vision` + HLG signaled 内容）。探测结果驱动
    只报 8.4 位还是同时报 8.1。
  - 请求 HLG 的 DV 选项被用户选中时：dynamicRangeMode=2 + 只报 8.4 位。
  - HDR 模式 UI：DV 8.1 条目旁新增 "DV 8.4"（仅探测通过时显示，沿用
    StreamSettings 动态重建 list_hdr_mode 的既有逻辑，注意 foundDolbyVision 分支）。
  - 解码器路由 #538 逻辑复用：协商 5 时同样配 `video/dolby-vision` + DvheSt。
- 合并顺序：common-c → vplus（与 8.1 相同）。

## 5. 阶段与判定

| 阶段 | 内容 | Go/No-Go 判定 |
|---|---|---|
| 0 | RPU 8.4 变体 dovi_tool 2.3.3 验证：HLG BL 会话参数生成 RPU bin，`info -s`/`info -f` 解析 + CRC 往返通过；确认 RPU 头无需按基层改写 | 通过才进 Phase 1 |
| 1 | 主机协商层 + 单测（优先级/矛盾上报/RTX HDR 互斥/旧客户端兼容） | CI 绿 |
| 2 | 主机编码注入 + 全量 test_sunshine | CI 绿 |
| 3 | common-c + vplus 客户端 | 编译 + 探测日志 |
| 4 | 真机端到端：仅 8.4 设备点亮，logcat `negotiated:5` + DV 解码器路由生效；8.1 设备回归不受影响 | 里程碑 |

### Phase 0 结论（2026-09-01，Go ✅）

用现有写入器（golden 配置：peak 1000/min 1/cll 800/fall 320/l5 16,16 + L1 9,3200,1500，
scene_refresh 两变体）生成 RPU bin，dovi_tool 2.3.3 验证：

- `info -s` 两变体均解析通过：Profile 8 / CM v2.9 / L1+L5+L6 完整。RPU 本身不含
  基层 EOTF 信息，"Profile: 8" 不区分 8.1/8.4。
- `editor` mode 4（dovi_tool 的 8.1→8.4 转换）逐字段对比：**只重写 rpu_data_mapping
  曲线（PQ 恒等映射 → HLG 域 8 段补偿曲线）与 CRC，L1/L5/L6 逐位不变** —— 该转换
  面向"PQ 素材转码为 HLG"的场景，补偿的是像素域转换。
- 我们的场景是**原生 HLG 编码**（分析器统计直接经 HLG OETF 出片），不存在需要补偿
  的域转换，恒等映射 + L1/L5/L6 正是原生 8.4 RPU 的正确形态。
- **结论：RPU 写入器零改动**；8.4 会话直接复用 `rpu_generator_t` 与注入链路。
  最终正确性由 Phase 4 真机（DV 引擎实际映射行为）收口。

## 6. 风险

- **RPU 对 HLG BL 的兼容性**（Phase 0 消解）：手写模板基于 8.1 实测 golden；
  若 dovi_tool 显示 HLG 源需要不同的 mapping 头，Phase 0 即停，重新评估。
- **客户端矛盾上报**（8.1/8.4 同报 + 请求 HLG）：主机按 colorspace_unsupported
  拒绝 8.4，HLG 会话直落**无动态元数据的普通 HLG**（HDR10+ 需 PQ，无中转），
  行为保守不炸。
- **旧主机 + 新客户端**：新客户端报 1<<4 位，旧主机 mask off，协商行为同现状。
- **真机覆盖**：Phase 4 需要一台"仅支持 8.4"的目标设备做正例；8.1 设备回归只需现有
  OPPO PKJ110 / Sony 电视。

## 7. 首发规格

- 协商格式：`dolby_vision_profile_84`（wire 5）
- 编码：HEVC Main10、bt2020hlg、P010、RPU 注入同 8.1 链路
- 互斥：app rtx-hdr=on 时 8.4 不协商
- 降级链：任一门失败直落当前 HLG 无动态元数据状态（HLG 会话无 HDR10+ 中转）
