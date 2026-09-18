# DLSS NR 编码前神经增强 filter 实现方案（形态 1：同分辨率 SDR）

> 2026-09-18。目标：在 pre-encode filter 链上新增 `alkaidlab.nvidia_dlssnr` 后端，把 DLSS 5 神经渲染（NGX Feature 18，DLSSNR）作为 SDR 会话的同分辨率画质增强 pass，与 RTX HDR 并列。导引契约完全复刻 Magpie-Experimental（SAOG0721 fork）已验证的 colour-only 方案：**零深度纹理 + NVOF 光流 MV + SEH 熔断**。

> **实施状态**：M1 已完成（2026-09-18）。契约层 `external_sdr_to_sdr_nr` + `resolve_frame_pipeline_policy` 第三参、identity 兜底 filter、host 侧 `dlssnr_filter`/adapter ABI/带 runtime pin 的 loader、schema v2 双能力槽管理面（v1 自动迁移 + 文件内 runtime pin）、apps.json `dlssnr` 节点、nvhttp/rtsp SDR 激活分支、display_vram 门控拆分均已落地。测试：frame_contract 13/13、pre_encode_filter 8/8、test_sunshine 配置+契约套件全绿（唯一失败 DownloadFileTest 为 httpbin.org 外网集成测试，与改动无关，干净树上同样失败）。
>
> **M2 已完成（代码侧）**：`dlssnr_adapter.cpp` 实现 snippet 加载（Init_Ext + appId 0x0876232C + IAT GetModuleFileNameW→"nvngx.dll" 调用方伪装 + 全调用 SEH 熔断）、私有 D3D12 设备（LUID 同卡）、NTHANDLE 共享纹理（BGRA8 输入/输出 mirror + R16G16 零 motion + R32 零 depth）+ D3D11↔D3D12 共享 fence、Feature 18 创建（Upscaling=0/Scale=1/Preset=0/Balanced）、逐帧 Evaluate（全幅 subrect、DepthInverted=1、风格参数透传、失败帧回退拷贝 input mirror）。MSVC 子构建 `sunshine_dlssnr_adapter`（无需 SDK 下载）+ 信任头（仅 pin adapter）+ 打包条目全部接线，主程序带 `SUNSHINE_DLSSNR_ADAPTER` 定义编译链接成功，smoke exe 编译成功。
>
> **M2 待办（阻塞于外部输入）**：真机 smoke 需要 `nvngx_dlssnr.dll`（本机未找到）。命令：`build\hdr_enhanced\nvidia_dlssnr_adapter\Release\foundation_dlssnr_adapter_smoke.exe <DLL 所在目录>`。关键验证点：BGRA8 直接喂 Color 是否被接受（拒则引入 swizzle pass）、AllocateParameters 是否由 snippet 导出、GPU 耗时。M3（NVOF 光流）尚未开始——process() 当前恒用 zero motion，motion_quality 参数已透传 config 但未接分支。

## 0. 参照物与证据基础

- 参照实现：`%TEMP%\magpie-exp`（SAOG0721/Magpie v0.6.x 克隆），核心文件：
  - `NgxD3D12Core.cpp` / `DLSSNRFilter.cpp`（1380-1455 signed snippet 加载、2100-2190 CreateFeature、2290-2480 Evaluate+同步）
  - `NvidiaOpticalFlowProvider.cpp`（498-506：`nvofapi64.dll` 系统目录加载 + `NvOFAPICreateInstanceD3D11`）
  - `docs/EXPERIMENTAL_HANDOFF_ZH.md`（交接文档，零深度契约与 FG fence 教训）
- 互操作配方（已从源码核实）：D3D11 侧 `MISC_SHARED_NTHANDLE` 纹理（UAV 场景加 `D3D11_BIND_UNORDERED_ACCESS`，DLSSNRFilter.cpp:1086-1109）→ `CreateSharedHandle` → D3D12 `OpenSharedHandle`；fence：D3D11 `CreateFence`（SHARED）→ D3D11 Signal → D3D12 queue Wait，反方向 D3D12 Signal → D3D11 Wait（2031-2044、2309-2427）。**全程无 keyed mutex**（单线程顺序提交 + GPU fence 保序）。
- 本仓库现成基建：pre-encode filter 链 + truehdr adapter 全套（见 §2 各节引用），管理面 hdr_enhanced 单组件设计。

## 1. 数据流总览

```
DDA 采集 (BGRA8 sdr_rec709, keyed mutex)
  └─ convert() display_vram.cpp:505-545（现有点）
       ├─ CopyResource → filter_handoff_texture（:519，无变化）
       ├─ 释放 encoder mutex（:520-522，无变化）
       ├─ pre_encode_filter->process()          ← 新 filter：
       │    ├─ D3D11: CopyResource input→input_mirror(NTHANDLE)
       │    ├─ D3D11 fence Signal → D3D12 Wait
       │    ├─ [NVOF] cur/prev luma → 光流 MV（R16G16_FLOAT，DLSS 约定）| 首帧/复位: 零 MV
       │    ├─ D3D12 command list: barriers → SetEvaluateParameters → NGX Evaluate(Feature 18)
       │    ├─ D3D12 Signal → D3D11 Wait
       │    └─ D3D11: CopyResource output_mirror→output(本帧 filter 输出纹理)
       └─ filter_result(sdr_rec709/unorm8) 替换 conversion_input（:540-544，无变化）
            └─ 现有 SDR 转换着色器 → NV12 → NVENC/AMF/QSV（零改动）
```

## 2. 分层改动

### A. 帧契约层 `src/platform/frame_contract.{h,cpp}`

- `pre_encode_filter_e`（h:36-40）新增 `external_sdr_to_sdr_nr`。
- `resolve_frame_pipeline_policy(dynamic_range, post_process_hdr_active)` 增加第三参 `post_process_nr_active = false`：
  - `dynamic_range == 0 && post_process_nr_active`：`output` 保持 sdr wire；`capture = { sdr_rec709, unorm8, require_private_handoff=true }`；`source_display` 保持 unchanged。其余分支行为不变（`post_process_hdr_active` 逻辑原样）。
  - 契约目的：让 display_vram.cpp:1311-1317 的 filter 创建检查通过（它要求 private handoff）。
- `postprocess_produces_hdr_output`（cpp:66-73）**不改**：NR 的 `output.transfer == sdr` 使其天然返回 false，`synthetic_hdr_source_active`/`get_effective_hdr_metadata`/`apply_client_target_luminance`/`colorspace_from_client_config` 全部不受影响（已核实 video.cpp:2196-2260、video_colorspace.cpp:50-58）。
- `pre_encode_filter_config_t`（h:42-47）追加 NR 参数字段（由 filter kind 标记语义）：`float nr_intensity=1, nr_local_tone_strength=1, nr_local_structure_strength=1; int nr_style=0; bool nr_auto_mask=false, nr_ui_correction=false; int nr_motion_quality=2;`（1=低/2=中/3=高，对应 NVOF 档位；已知债务：与 truehdr 参数共用结构体）。

### B. filter 基建 `src/platform/windows/pre_encode_filter.*`

- `pre_encode_filter_helpers.h` 新增 `make_sdr_result(view, output_texture, output_srv)`：语义 = 拷贝 input.semantic 后强制 `domain=sdr_rec709, encoding=unorm8, borrowed=false`，format = 输出纹理实际格式。与 `make_scrgb_result` 并列。
- 新增 `identity_sdr_filter_t`（~30 行，直接放 pre_encode_filter.cpp）：`process()` 原样返回输入 view（status=ready）——作为 NR 主 filter 的 failover 兜底，零拷贝。
- `make_pre_encode_filter` 分发表（cpp:246-290）新增 `external_sdr_to_sdr_nr` 分支：primary = `hdr_enhanced::nvidia_dlssnr::make_filter(...)`（经 hdr_backend_factory），fallback = identity filter。

### C. adapter DLL（核心新增组件）

目录 `src/platform/windows/hdr_enhanced/nvidia_dlssnr/`：

```
adapter_abi.h                 C ABI（镜像 truehdr 模式）
adapter/CMakeLists.txt        MSVC 独立子构建（镜像 rtx_video_adapter.cmake 模式）
adapter/src/dlssnr_adapter.cpp    create/process/flush/destroy + SEH + D3D11↔D3D12 互操作
adapter/src/ngx_snippet.h/.cpp    LoadLibrary("nvngx_dlssnr.dll") + 自声明原型 + DLSSNR.* 参数串常量
adapter/src/nvof_provider.{h,cpp} NVOF 光流（Magpie provider 移植，D3D11 接口）
adapter/src/guidance.{h,cpp}      luma 抽取 CS（BGRA→半分辨率 luma）、MV 格式转换、零 MV 兜底
adapter/tests/adapter_smoke.cpp   硬件 smoke（镜像 truehdr smoke）
dlssnr_filter.{h,cpp}         宿主侧 pre_encode_filter_t 实现（镜像 truehdr_filter.cpp）
```

**ABI**（v1）：`foundation_dlssnr_adapter_get_api`，`create(void* d3d11_device, const foundation_dlssnr_config_t*, void** instance)`、`process(instance, ctx, input_tex, output_tex)`、`flush`、`destroy`。config：`struct_size/width/height` + 上述 7 个 NR 参数 + `motion_mode(none|optical_flow)` + `runtime_directory`。导出函数全部 `noexcept + catch(...)`，异常不穿 ABI（truehdr_adapter.cpp:274-340 先例）。

**NGX 加载（只走 signed-snippet 路径，不用 SDK 主入口）**：
- `LoadLibraryExW(runtime_directory / "nvngx_dlssnr.dll", LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | DEFAULT_DIRS)`（DLSSNRFilter.cpp:1384-1392 同款，同目录依赖 nvngx_dlss.dll 一并解析）。
- GetProcAddress：`NVSDK_NGX_D3D12_Init_Ext / CreateFeature / EvaluateFeature / ReleaseFeature / Shutdown1`。原型自行声明（稳定 C ABI），**不链接 nvsdk_ngx_d.lib、不进 NVIDIA 头文件**。
- 初始化顺序（Initialize，镜像 DLSSNRFilter.cpp:1990-2190）：encode device 取 LUID → `D3D12CreateDevice` 同卡私有设备 → snippet `Init_Ext(appDir, device12)` → `AllocateParameters` + create 参数（`DLSSNR.Upscaling=0/Scale=1/ScalingRatio=1+callback/Preset=0/PerfQuality=Balanced/NodeMask=1`，1345-1368 同款）→ 携 init command list `CreateFeature(feature=18)` → 构建互操作资源。
- **互操作资源**（复刻 1086-1109/2031-2044 配方，均在 encode D3D11 device 上创建）：
  - `input_mirror`：BGRA8 UNORM，`BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS` + `MISC_SHARED_NTHANDLE`，D3D12 侧开成 Color 输入；BGRA→RGBA8 需要时用 4-tap swizzle CS（Magpie COLOR_CONVERT_HLSL 同理；若 NGX 直收 BGRA 则省略，smoke 实测定）。
  - `output_mirror`：同 flags，D3D12 侧为 Output（UAV）。
  - `zero_depth`：R32_FLOAT 全零（zero-contract，handoff 文档确认 SDK 合同只要求资源存在）；`zero_motion`：R16G16_FLOAT 全零。
  - 共享 fence ×1（双向）。
- **process() 逐帧**（复刻 Draw 2290-2440 顺序）：
  1. 校验 ctx 同源 + immediate（truehdr_adapter.cpp:182-192 先例）、输入格式 ∈ {B8G8R8A8/R8G8B8A8}_UNORM、尺寸 == config。
  2. `CopyResource(input_mirror, input)`；D3D11 Signal → Flush → D3D12 Wait。
  3. [motion=optical_flow] luma 抽取（半分辨率）→ NVOF cur/prev 执行 → MV 转成 DLSS 约定（符号/Y 翻转按 Magpie guidance glue 复刻，编码期对照其源码校准）→ 写 `motion_tex`；首帧/分辨率变化/复位 → zero_motion。MVec/Depth Subrect 全幅，`MVecScale=1`，`DepthInverted=1`，`IndicatorInvert=0/0`。
  4. D3D12 command list：barriers（Color→NON_PIXEL_SHADER_RESOURCE、Output→UAV）→ `SetEvaluateParameters`（`DLSSNR.Color/Output/MVec/Depth` + 7 风格参数 + `Reset` 条件：输入 revision 变化或 NVOF 会话复位）→ `EvaluateFeature` → 反向 barriers → Close → Execute → queue Signal。
  5. D3D11 Wait(fence) → `CopyResource(output, output_mirror)`。
  6. **每次 NGX 调用包 SEH `__try/__except`**；fault → 实例标记 disabled、本次返回 `INTERNAL_ERROR` → 宿主 failover 永久切 identity（复刻 NgxRuntimeGuard 语义：fault 后到 destroy 前不再触碰 NGX）。
- **失败即降级、不自动恢复**：与 failover_filter_t（pre_encode_filter.cpp:164-223）现有语义一致。
- 每帧 GPU 计时：D3D12 timestamp query 环形槽（复刻 2355-2375、2454-2475 的 timing 结构），写日志。

**NVOF provider**（nvof_provider.cpp，移植自 Magpie 908 行 provider）：
- `LoadLibraryExW(L"nvofapi64.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)` → `NvOFAPICreateInstanceD3D11`；驱动自带，不链接不分发。
- 会话：encode device 上的 D3D11 NVOF，半分辨率执行（HalfResOpticalFlow 同思路），质量档映射 nr_motion_quality。前一帧 luma 缓存在 adapter 内（process 只收当前帧，ABI 不变）。
- 不可用（非 N 卡驱动/老驱动）→ 自动回落 zero-MV 并记日志，不失败整个 filter。

### D. 编码设备 `src/platform/windows/display_vram.cpp`

- init() filter 创建块（1304-1331）拆分：
  - `external_sdr_to_hdr`：维持现状（10bit HDR 编码面硬检查 1305-1310）。
  - `external_sdr_to_sdr_nr`：跳过 10bit 检查（NV12/AYUV 会话允许）；契约检查（1311-1317）不变——policy 层已保证 `require_private_handoff=true`。
- convert()（505-545）：零改动。状态上报（1741-1753）增加 `nr_backend/nr_state/nr_failure_reason` 三字段（与 synthetic_hdr_* 并列）。

### E. 激活链路

- `src/nvhttp.cpp` make_launch_session（283-290 后）：新增独立分支——`proc.get_app_dlssnr_config(appid)`（`process.h` 加接口，数据源 `app_t::dlssnr = std::optional<dlssnr_config_t>`，镜像 `app_t::rtx_hdr` 全链：process.h:75/115、process.cpp:504-516）→ enabled 且 `!enable_hdr`（SDR 专用，v1）→ `launch_session->dlssnr_backend = hdr_enhanced::manager().acquire_selected(NR_CAPABILITY)` + 填 `launch_session->dlssnr_params`。新增 launch_session_t 字段（rtsp.h，镜像 hdr_backend:82）。
- `src/rtsp.cpp`（1579-1608）：`dynamicRange == 0` 分支新增：`nr_active = session.dlssnr_backend && app 配置 enabled` → `monitor.pre_encode_filter = external_sdr_to_sdr_nr`、`monitor.hdr_backend = session.dlssnr_backend`、`monitor.pre_encode_filter_config = session.dlssnr_params`、policy 用 `resolve_frame_pipeline_policy(0, false, nr_active)`。PQ/HLG 分支不触碰。
- `src/video.cpp` strip_unusable_pre_encode_filter（3590-3607）：**零改动**——`disp.is_hdr()` 时摘除对 NR 同样正确（v1 明确只支持 SDR 显示器）；`supports_pre_encode_filter` 已由 display_vram_t=true 覆盖。

### F. 管理面多组件化（hdr_enhanced 升级）

- **schema v2**（`hdr_enhanced.json`）：
  ```json
  { "schema_version": 2,
    "selected": { "hdr": "alkaidlab.nvidia_rtx_video" | null,
                  "nr":  "alkaidlab.nvidia_dlssnr"  | null },
    "backends": { "<id>": { "version": "...", "runtime_sha256": "..." | null } } }
  ```
  - v1 文件读取后自动迁移（selected_backend→hdr 槽）；写回一律 v2。
  - `runtime_sha256` 由控制面板导入时计算写入（泄露 DLL 无构建期已知哈希；文件内 pin 防导入后被换，威胁模型内自洽），`null` = 仅记录日志不 pin。adapter 哈希仍走构建期信任目录录（GenerateRtxVideoTrustHeader.cmake 扩展成双组件）。
- `src/hdr_enhanced/config.{h,cpp}`：
  - 常量区加 `NVIDIA_DLSSNR_BACKEND/"alkaidlab.nvidia_dlssnr"`、adapter/runtime 文件名。
  - 新增能力槽概念：`backend_capability_e { hdr, nr }`；`acquire_selected(capability)`；`parse_settings` 校验两个已知 id、`selected` 槽结构；`impl_t::validate` 目录表驱动（`id → root/hdr_enhanced/<subdir>`），dlssnr 的 runtime 校验接受文件内 pin 或 null（null 时仅算哈希记录）。
  - 现有 truehdr 单测（test_hdr_enhanced_config.cpp）补 v2 迁移用例。
- `src/hdr_enhanced/api.cpp` + `confighttp.cpp`：
  - `get_status` 的 `runtime["pipelines"]` 追加 nr_* 字段透传。
  - maintenance 路由正则泛化 `^/api/hdr-enhanced/components/([a-z0-9_.-]+)/maintenance$`，id 白名单 = 已知 backend 集合；config/save 路由不变。
- Tauri 控制面板：
  - `src-tauri` `mod.rs`：`RTX_HDR_RUNTIME` 常量改 per-component 表；`native_component_import/remove` 按 componentId 分派目录与文件名；capability `main-hdr-enhanced.json` 增补 dlssnr 命令。
  - `tauri-adapter.js`：新增 `dlssnr` 绑定（import sources: `nvngx_dlssnr.dll`，含同目录伴随 DLL 提示）。
  - 面板 UI：hdr-enhanced 视图增加第二张卡（DLSS NR：启用/版本/导入 nvngx_dlssnr.dll/状态），沿用控制器中心设计语言（投影大按钮/中括号小按钮），仅设备中心可见，不进公共视图（既有约定）。
  - locale：en/zh 两份 key。
- app 级配置：Sunshine Web UI 每个 app 增加 "DLSS NR" 开关与参数（进阶项），存 `apps.json` 的 app_t::dlssnr，序列化沿用 rtx_hdr 模式。

### G. 构建与打包

- `cmake/dependencies/dlssnr_adapter.cmake`：开关 `SUNSHINE_DLSSNR`（AUTO/ON/OFF），镜像 rtx_video_adapter.cmake 但**无需任何 SDK 下载/secrets**（原型自声明，仅链 d3d11/d3d12/dxgi）→ CI fork 也可构建；产物 `foundation_dlssnr_adapter.dll`；信任头生成器只 pin adapter 自身哈希。
- `cmake/targets/common.cmake`：存在目标时主程序加 `SUNSHINE_DLSSNR_ADAPTER` 定义。
- 打包：`cmake/packaging/windows.cmake` + `sunshine.iss.in` 增加 `tools/hdr_enhanced/nvidia_dlssnr/` 目标；**nvngx_dlssnr.dll 及伴随 DLL 永不入包/不入 CI 产物校验白名单**（新增负向断言，镜像 main.yml:313-333 的 truehdr 校验）。
- 签名流水线 sign-and-repackage.yml 增加 adapter DLL 条目（镜像 249-252）。

## 3. 里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 契约与链路 | §2-A/B/E + identity fallback + per-app 配置 + M4 的最小 schema v2 后端（先无面板） | 单测绿；SDR 会话挂 identity filter 全链无回归 |
| M2 adapter 零 MV | §2-C 主体（snippet+互操作+Evaluate，motion=none）+ smoke | smoke 通过（5090 实机）；真机 Moonlight SDR 串流开启 NR 出图，GPU 计时入日志 |
| M3 NVOF | nvof_provider + guidance | 运动场景对比零 MV（录屏对比 + 拖影/闪烁评估）；NVOF 缺失自动回落 |
| M4 管理面完整 | §2-F 全量 + 打包/CI | 面板导入 DLL→启用→串流生效全流程；泄露 DLL 不在包内的 CI 断言 |

M1-M2 是画质可行性的决定点；M3/M4 可并行。

## 4. 风险与对策

1. **泄露 DLL 无官方渠道**：adapter 与运行时 ABI 面按 Magpie 已验证行为编写；版本探测（DLL 版本资源 + NGX 结果日志）与已知良好版本 pin（310.8）写进文档；不入仓库、不入包。
2. **feature 18 契约随版本变化**：所有 NGX 参数串集中在 ngx_snippet.h 一个头；init/evaluate 失败一律熔断降级 identity，串流永不断流。
3. **immediate context 阻塞**：D3D11 Wait 是 GPU 侧 fence 等待，CPU 不空转；process 在编码线程串行，无锁竞争。延迟预算目标 ≤3ms@4K（M2 实测，超预期则把 input 拷贝与 NVOF 挪前一帧槽位）。
4. **NVOF 符号约定踩坑**：MV 方向/Y 翻转以 Magpie guidance glue 为准移植，smoke 用已知平移序列帧验证（输出 MV ≈ 位移）。
5. **GPLv3 + 专有组件**：同 RTX HDR 现状（adapter 侧载 + 运行时用户自备 + THIRD_PARTY_NOTICES），法务姿态不变。

## 5. 验证

- 单测：`test_frame_contract`（NR policy 分支）、`test_pre_encode_filter`（NR 工厂 + identity failover，stub 模式）、`test_hdr_enhanced_config`（v1→v2 迁移、双 id、runtime pin 三态）。
- smoke：`foundation_dlssnr_adapter_smoke.exe <runtime_dir>`——1080p 渐变帧 ×N：断言 status==OK、输出有限非零、平移序列 MV 方向正确、100 帧计时分布。
- 真机（5090 主机 + Moonlight）：SDR 1080p/4K60 各一档——开关对比截屏、GPU 计时（日志 timing 行）、端到端延迟（无变化预期 +1ms 内）、长时间会话稳定性（30min 无 fallback 触发）、非 N 卡/驱动缺失时静默回落。
- 明确不做（本期）：超分（采集低分→编码高分）、DLSS FG、HDR 显示器输入、双卡分离、多 pass 串联、FP16 scRGB 路径——均记为后续形态候选。
