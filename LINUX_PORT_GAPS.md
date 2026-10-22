# Linux 移植遗留差距清单

- **基线**：`linux-migration` 分支（pkgrel 84，2026-10-22；已并入上游 `origin/master` `3c194184`，
  上一个同步点是 `ed415026`）
- **对照**：fork Windows 版（AlkaidLab master `3d76d35d`）的全部增强功能
- **用途**：**结论清单**（2026-10-22 起）。可做的都已落地或已评估收束；每项含代码位置、证据与
  **重启判据**（什么条件变化才值得重新开启）
- **约定**：✅ 已完成｜⏸ 已评估收束（附重启判据）｜⛔ 本质不可移植｜○ 等上游定义协议
- **已完成项**：见 `LINUX_MIGRATION_REPORT.md` §六，本文档不再复述

---

## 总览

核心体验链路已全部对齐：虚拟显示器（原生 EDID 后端 + 配置全组合模式表 + prep 模式 + 独占还原 +
残留 adopt）、物理显示器 display_device（kscreen/niri 分辨率/拓扑 + 持久化还原）、HDR 三件套
（静态透传 / HDR10+ 动态 / DV P8.1 RPU / Vivid）、剪贴板文本+图片双向（KDE 走 klipper+合成器，
非 KDE 桌面走合成器）、远程麦克风、ABR 前台检测
（KDE + niri）、托盘/向导/打包。

**剩余待办：0 项。** 终局分布：
**已评估收束 6 项**（§一 §1.5 帧管线契约 / §1.7 音频位深；§二 §2.7 DS5 sidecar / §2.8 NVENC 直连 /
§2.9 Tauri 面板 / §2.11 VAAPI 4:4:4 —— 每项都写明证据与"什么条件变化才重新开启"）、
**1 项等上游定义协议**（§2.1b 剪贴板 file-offer，本树两端都没有生产者）、
**9 项本质不可移植**（§三，含 Windows 对应物与 Linux 现状）。
判定依据分三类：**本机没有验收对象**（无 DS5 硬件 / 无 wlroots、GNOME 会话）、
**本机驱动没有该能力**（nvidia VAAPI 只解码）、
**收益已被替代路径吃掉**（WebUI 覆盖面板、avcodec 覆盖 NVENC 直连、PipeWire 无损转换覆盖位深）。
（2026-10-22 告一段落、已从待办中移除：§1.2 HDR 分析精度（逐像素 maxRGB）、§1.3 剪贴板图片 + 非 KDE
桌面 provider、§1.6 / §2.6 AI Key 的 Secret Service 后端、§2.5 非 KDE
前台检测、§2.1 剪贴板图片双向同步、§2.2 USB/IP 主机后端、§2.3 GNOME Mutter 显示后端（含复制
拓扑）、§2.4 DSU 运动广播、§2.10 显示后端四项（niri + wlroots/wlr-randr + X11（RandR）+ KDE 复制
拓扑）、§7.1 管线帧率的"源更新"段、
§1.1 虚拟显示器 per-client 隔离（阶段一客户端身份进 EDID + 阶段二并行多实例）、
§7.3 依赖系统已安装资产的单测（改成按资产是否存在分支断言）、
§7.2 依赖外网的单测（复核确认上游已自行摘出 ctest 门禁）。
另有**有意保留的差异**（§四，决策记录）、**上游新特性的 Linux 缺口**（§五），以及
**2026-10-22 上游同步（`ed415026..7a0995e2`，33 个提交）新引入的缺口**（§七，其中 §7.1/§7.2/§7.3
已了结，§7.4 记录了三个 SetUpTestSuite 失败的根因——测试二进制没有 `cap_sys_admin`，非缺口）。

---

## 一、显示与音频的收束项（✅ 完成 / ⏸ 已评估收束）

### 1.1 ✅ 虚拟显示器 per-client 隔离（2026-10-22 完成，阶段一 + 阶段二）

- **阶段一（客户端身份进虚拟屏）**：`src/vdd_client_identity.{h,cpp}` 是共享的规范实现
  （RFC 4122 v5 / SHA-1 / nil 命名空间），Windows 交给 ZakoVDD 的 GUID 与 Linux 写进 EDID 序号的
  32 位串号**同源派生**，所以两个平台对"这是哪个客户端的虚拟屏"判断一致；产品名仍是 `ZAKO_NAME`，
  客户端身份不外泄到用户可见命名。
- **阶段二（并行多实例，2026-10-22 落地）**：后端状态从单实例改成实例表。
  - `src/platform/linux/vdd_instances.{h,cpp}` 只装**纯规则**（无 DRM、可单测）：
    `find()`（按 EDID 串号定位请求所属记录；无身份的调用方退回"选中记录"，即历史单槽行为）、
    `live_count()`、`used_connectors()`、`pick_connector(candidates, unavailable)`。
  - `src/display_device/vdd_utils.cpp` 保留共享的 `active_*` 会话字段，但新增
    `std::vector<instance_t> instances` + `selected_instance`，并以
    `store_current_locked()` / `load_instance_locked()` 在"请求态"与"某台虚拟屏的态"之间搬运；
    `adopt_orphan_vdd_locked()` 现在会把**所有**活的虚拟连接器都收养进表（原来至多一台）。
  - `create_vdd_monitor(identifier, …)`：先 `store_current_locked()` → 该客户端**自己**的活屏直接
    复用（连 EDID 个性化与合成器布局都不动）→ 否则选一个空闲连接器（排除表中已占用的连接器，
    以及独占会话关掉的物理输出——它们在 sysfs 里也只是 "disconnected"，无从分辨）→ 只清理
    **无主**的残留虚拟屏 → 建新记录并选中。
  - `destroy_vdd_monitor(identifier)` / `destroy_vdd_monitor_nolog(identifier)`：只拆自己那台；
    "物理屏复位 + 主屏交还"只在**最后一台**虚拟屏消失时执行，仍有客户端在串流时不会把物理屏
    重新点亮。`src/display_device/session.cpp` 的 `session_t::destroy_vdd_monitor()` 把
    `current_vdd_client_id` 传下去，重建/恢复路径统一用同一个 `vdd_identifier`
    （`vdd_reuse` 时是 `shared_vdd`，否则是客户端标识符），因此不会再误拆另一台。
- **平台中立性**：`destroy_vdd_monitor*` 新增的 `std::string_view` 参数在 Windows 半边显式
  `(void)` 忽略（Windows 由驱动按客户端 GUID 管理虚拟屏，本来就有真正的隔离），Windows 行为逐字不变。
- **上限**：受空闲连接器数量约束（本机断开态连接器只有 `DP-1` 与 `HDMI-A-1`，`eDP-1` 是真实内屏
  不能占；独占会话还会把物理输出排除掉，此时实际只剩一两个可占）。
- **验收**：`tests/unit/test_vdd_instances.cpp` 9 例全绿——同一标识符定位到自己的记录、收养来的屏
  靠串号也能认领、陌生客户端**不会**继承别人的屏（回归就是这个）、无身份调用保持单槽行为、
  空闲连接器按序分配、独占关掉的物理输出被跳过、两客户端最终落在不同连接器上。
  合并后全量 `test_sunshine` 821 例 / 808 通过 / 12 跳过 / 1 项环境依赖失败（§7.2，已被 ctest 门禁过滤），无新增失败。

### 1.2 ✅ HDR 亮度分析精度（2026-10-22 完成，走 CPU 而非 GPU）

- **做法**：新增 `src/video_hdr_analyzer.h`（纯 header 的 `video::hdr_analyzer`），把原来"拿亮度当
  maxRGB"的直方图改成**逐像素 max(R,G,B)**：按 BT.2020 NCL（显式 BT.709 系标号时才换矩阵）反
  矩阵把 Y'CbCr 还原成 R'G'B' 的 PQ 信号，取三者最大再落回 10-bit 码进直方图。4:4:4 与 4:2:0
  都在内（4:2:0 按最近邻取同址色度），P010/P16 的 MSB 对齐与 planar P10 的 LSB 对齐统一由一个
  `shift` 处理。`src/video.cpp` 只负责把 `AVFrame` 填成 `hdr_analyzer::frame_t` 再调用。
- **为什么不用 CUDA**：这条分析每 4 帧对一张**已降采样**的帧跑一次，CPU 逐像素 6 次乘加在分析
  分辨率下可以忽略；CUDA 路径要把 scRGB 的 hwdevice 与 VAAPI/软件编码两条链路都做成条件分支，
  收益不抵复杂度。PQ 数学仍只由 `video_hdr_metadata.h` 提供，没有第二份实现。
- **顺带修正的契约问题**：`platf::hdr_frame_luminance_stats_t` 的字段注释一直写的是 max(R,G,B)
  （`src/platform/common.h:527-549`），旧实现喂的是亮度，属于实现与文档不符；现在两边一致。
- **实测**：纯红 100 nits 的帧，旧实现（亮度）给 ~1.2 nits，新实现给 100 nits（见单测）。
- **验收**：`tests/unit/test_video_hdr_analyzer.cpp` 10 例 —— 纯红不再是亮度值、灰阶仍等于亮度、
  有限/全范围一致、MSB/LSB 对齐一致、4:2:0 色度覆盖到每个像素、无色度回退亮度、黑帧
  near-black 覆盖率为 1、坏帧不发布零值、序列号逐帧递增、BT.709 走自己的矩阵。

### 1.3 ✅ 剪贴板图片 + 非 KDE 桌面 provider（2026-10-22 完成）

- **现状**：`src/platform/linux/clipboard_data.{h,cpp}`（`ext_data_control_v1`）双向承载
  **图片与文本**。图片：PNG 发布/接收（`/tmp` 三尺寸实测 md5 一致）。文本：原先只有 klipper
  一条路，非 KDE 桌面（niri/sway/Hyprland）上双向都不工作；现在启动时探测 klipper
  （`klipper_present()`，一次 `klipper_get` 探针），**缺失且 data-control 可用时自动改走合成器**
  （`ext_text`），发布/接收都带 UTF-8 mime 协商（`src/platform/linux/clipboard_mime.h`：
  `text/plain;charset=utf-8` → `UTF8_STRING` → `text/plain`，非 UTF-8 字符集直接拒收而不是转码
  成乱码）。真机实测：`set_text` → `wl-paste` 读到同一字符串；外部 `wl-copy` → sink 收到。
  mime 策略由 `tests/unit/test_clipboard_mime.cpp` 10 例覆盖。
- **仍缺**：`KIND_FILE_OFFER`（主机侧写 uri-list）→ 见 §2.1b；klipper 仍优先于合成器（KDE 上文本
  仍走 klipper，避免两套文本源互相覆盖）。
- **验收（已达成）**：Plasma 6 下手机/客户端复制截图 → 主机粘贴为图片；无 klipper 的桌面复制文本
  → 主机粘贴得到同一文本。

### 1.4 ✅ 复制拓扑 + 非 KDE 桌面显示后端（2026-10-22 完成，五个后端）

- **现状**（✅ 2026-10-22 完成）：三后端已齐 —— kscreen-doctor（KDE Plasma）、niri IPC（§6.16）、
  Mutter DisplayConfig（§2.3）。分发顺序 `niri → mutter → kscreen`，都不在时退化为 no-op 并打
  一条 warning（不破坏）。
  - **复制拓扑（两个平台都已支持）**：
    - Mutter：一个 logical monitor 可含多个 connector，`set_topology` 直接写多设备组（§6.18）。
    - KScreen：**`kscreen-doctor` 其实有镜像命令** —— `output.<name>.mirror.<other|none>`
      （`kscreen-doctor --help` 里明写，旧版本文档"没有 replication 命令"的判断是错的，凭
      `output.<name>.mirror.eDP-1` 即可）。`apply_kscreen_mirroring()` 在使能/禁用之后追加这一步：
      成员指向组首（`mirror.<source>`），不再需要镜像的输出显式回收（`mirror.none`，否则残留关系
      不会被清除）；具体要发哪几条由纯函数 `src/platform/linux/mirror_plan.h` 算出。
    - niri 没有镜像概念，`compositor_supports_replication()` 对它返回 false，多设备组仍被校验层
      拒绝（早失败、报错信息一致）。
  - **统一读取**：`get_current_topology()` 用 `compositor_output_t::replication_source`（KScreen 与
    Mutter 都填"源输出的数字 id"）把镜像成员折叠回一个组，所以三后端的拓扑回读、`is_topology_the_same`
    校验、`set_topology` 的自动回退都是同一套逻辑。
- **验收**：`tests/unit/test_mirror_plan.cpp` 8 例覆盖"已镜像/新镜像/回收/改指向/交换方向/禁用输出/
  未知输出/扩展桌面不变"；命令形态已在本机 KDE 上实测（`output.eDP-1.mirror.DP-2` → 回读
  `replication source: 1`，随后 disable 复原）。遗留的只是"真镜像下画面是否一致"这层观察。

### 1.5 ⏸ 帧管线契约（frame_contract）——已评估，不移植（2026-10-22）

- **结论**：不移植。契约的策略层是跨平台的，但它在 Linux 上**没有可挂的前置滤镜实现**，单独
  移植只能得到永远为 none 的管线策略。
- **证据（本次复核）**：
  - 契约本身是共享层：`src/platform/frame_contract.cpp` 107 行 + `.h` 125 行，由
    `src/platform/common.h:29` 引入；`src/video_colorspace.cpp:58`、`src/rtsp.cpp:1844/1861`、
    `src/image_enhancement/nr_defaults.h:5` 都在共享层消费它。
  - 采集端能力开关 `platf::display_t::supports_pre_encode_filter()` 的基类返回 **false**
    （`src/platform/common.h:758`），**唯一 override 在 `src/platform/windows/display.h:383`**；
    `src/video.cpp:4317` 一旦看到 false 就关掉滤镜并重解管线策略。
  - 两个真实滤镜 `external_sdr_to_hdr` / `external_neural_enhancement` 的实现是
    `src/platform/windows/pre_encode_filter.cpp`（D3D11 计算着色器路径），属 §三 不可移植清单。
- **重启判据**：上游把 pre_encode_filter 抽成带平台后端的接口（出现非 Windows 的
  `supports_pre_encode_filter()` 实现），或 Linux 出现等价的 GPU 端前置滤镜需求。


### 1.6 AI API Key 加密存储（✅ 已完成，2026-10-22）

- **已做**：`src/ai/credential_store.cpp` 在 `SUNSHINE_BUILD_LIBSECRET` 下走 Secret Service
  （KWallet 的 secret-service 兼容层 / gnome-keyring）。条目是每用户一条，schema
  `org.lizardbyte.sunshine.LlmApiKey` + 属性 `application=sunshine,purpose=llm-api-key`，
  label `Sunshine LLM API key`，落在默认 collection。
- **读取顺序**：keyring → `SUNSHINE_LLM_API_KEY` 环境变量兜底（与 Windows 一样，持久化值优先，
  env 只做引导）。
- **擦除**：清 keyring 条目；若 env 仍设着则报错提示先清理 service 环境（否则 key 仍在生效）。
  清空不存在的条目视为成功（幂等）——libsecret 对"无匹配"返回 FALSE 且**不带 GError**，这点已实测。
- **构建**：`cmake/compile_definitions/linux.cmake` 里 `pkg_check_modules(LIBSECRET libsecret-1)`
  可选发现，找不到就退回纯环境变量（`message(STATUS "libsecret not found …")`），Windows 半边不变。
- **验收**：`tests/unit/test_credential_store.cpp` 的 `RoundTripsThroughTheKeyring` 通过
  （write→read→空写擦除→not_found→erase 幂等）；无 Secret Service 或已有用户 key 时 `GTEST_SKIP`，
  不会覆盖真实凭据。
- **残留风险**：若 collection 处于锁定状态，libsecret 的同步调用可能等解锁提示而阻塞调用线程
  （本机 KWallet 未锁，实测 write/read/erase 合计 16 ms）。真出现再考虑加超时/异步。

### 1.7 ⏸ 音频设备位深匹配——已评估，不处理（2026-10-22）

- **结论**：不处理。差异真实存在，但代价是一次**无损**转换。
- **证据（本次复核）**：
  - Linux：`src/platform/linux/audio.cpp` 建的虚拟 sink 固定 48 kHz / float（`format=float`，
    `PA_SAMPLE_FLOAT32`），麦克风混音侧同样是 float32（`Audio.cpp` 的 `pa_sample_spec`）。
  - Windows：`src/platform/windows/audio.cpp:87-122` 用 PolicyConfig COM 按客户端位深建设备
    （`wBitsPerSample` 16/24/32 + `WAVE_FORMAT_EXTENSIBLE`），为的是避免 WASAPI 重采样。
  - Linux 侧转换发生在 PipeWire 服务端，float32 正是服务端原生格式 ⇒ 无量化损失，只是多一次
    内存内的采样格式转换。
- **重启判据**：实测出现可听的失真（例如 24-bit 客人在 float 往返后出现截断伪影），或某类设备
  （如独占直通）拒收 float 格式。


## 二、可行项的去向（✅ 完成 / ⏸ 已评估收束）

### 2.1 剪贴板图片（✅ 图片已完成，2026-10-22；file-offer 拆分到 §2.1b）

- **落地**：`src/platform/linux/clipboard_image.{h,cpp}`（`SUNSHINE_BUILD_WAYLAND` 下编译）用标准化
  的 `ext_data_control_v1` 在独立连接/线程上双向同步 PNG，与 §6.4 的 klipper 文本半边共用
  `clipboard_wire` 帧。传输全部由事件循环 pump（非阻塞 fd + 10 s 截止），回调里不阻塞——否则
  `send()` 会与自己的阻塞读互锁。
- **能力通告**：`src/rtsp.cpp` 的 `clipboard_image` 改由 `clipboard_host::image_available()` 决定，
  合成器没有 data-control 协议时仍不通告（wlroots 系目前只导出 wlr 拼写，保持 text-only）。
- **实测**：594 B / 45 KB / 3.4 MB 双向 bytes 与 md5 一致（`wl-copy`/`wl-paste` 对照）。
- **验收**：客户端复制图片 → 主机 `wl-paste --type image/png` 得到同一 PNG；主机复制图片 → 客户端
  粘贴得到图片。✅

### 2.1b 剪贴板 file-offer（⏸ 等上游——本树里两端都不存在，不是移植缺口）

- **勘察结论（2026-10-22）**：`kKindFileOffer`（kind 4）在宿主 C++ 里**既没有生产者也没有消费者**
  ——`src/clipboard_wire.h:54` 的 `is_known_kind()` 认这个 kind，除此之外整棵树没有任何地方发它
  （`grep -rn kKindFileOffer src/` 只有 `clipboard_bridge.h:58` 的定义）。它的生产者是 Windows 的
  **GUI agent（Tauri 面板）**：`src_assets/…/tauri/src/file_transfer.rs` 的
  `post_file_offer_payload()` 往 **`POST /api/v1/file-transfer/offers`** 发请求，再把手柄贴进
  Sunshine 的剪贴板通道；而**这个端点在本树里同样不存在**。也就是说 Windows 侧端到端也依赖仓外组件。
- **因此**：原先写的"接 xdg-desktop-portal FileChooser"是错的——portal 只能表达"让用户挑文件"，
  而真实语义是"把主机已选中的文件交给客户端"，生产者属于 agent，不是宿主。
- **结论**：等上游先定义宿主侧协议与端点；在那之前列进待办只会误导。Linux 侧要做也只能做
  "读合成器选择里的 `text/uri-list` → 组帧"这半边，协议未定义时无从对齐。
- **验收**（若上游补齐）：客户端粘贴主机文件 → 得到文件句柄而非提示不支持。

### 2.2 USB/IP 主机（✅ 后端已落地，2026-10-22；部署需 root）

- **现状**：`usbip_host_backend` 增加 `usbip_linux` 方言，`automatic` 在 Linux 解析到它。控制器
  （`src/remote_usb/remote_usb_host_controller.cpp`）对两种方言共用同一套生命周期/补偿逻辑，只换
  argv 与端口来源：
  - attach：`usbip --tcp-port PORT attach -r 127.0.0.1 -b BUSID`（`--tcp-port` 是全局选项，必须在
    子命令前；没有 win2 的 `--once/--terse`）
  - detach：`usbip detach -p PORT`
- **端口来源**：usbip-utils 成功时**不输出任何东西**，所以端口从内核 vhci 的
  `/sys/devices/platform/vhci_hcd.0/status` 反推：attach 前快照已占用端口，attach 后取差集（探针
  略滞后，50 ms × 20 次有界轮询）。取差集的窗口用文件级互斥串行化，避免并发 attach 互相认领同一端口。
  拿不到新端口就报 `attach_failed` 而不是返回一个无法 detach 的绑定。
- **端口编号**：vhci 端口是 **0 基**（`pdev_nr * VHCI_PORTS + i`），而绑定沿用 win2 的 1 基校验，
  因此内部存 `port + 1`，detach 时再减一。端口 0 与 15 都有单测覆盖。
- **可用性判定**：`backend_supported()` 要求 ① `usbip` 可执行文件存在（`usbip` 在 Arch 是独立包
  `extra/usbip`，属 linux-tools）② vhci 状态文件可读。任一缺失就回到 `unsupported`，reverse tunnel
  保持关闭而不是在 attach 时才炸。
- **部署前置（重要）**：vhci 的 `attach`/`detach` sysfs 属性**只有 root 可写**，而本仓打包的 service
  以用户身份运行（文件能力只含渲染相关）。因此远程 USB 实际启用需要二者之一：
  ① `sunshine` 以 root 运行（系统级 unit）；② 给 `usbip` 配 sudoers 并把 `usbip_host_controller_config`
  的 `executable` 指向包装脚本。权限被拒时 detail 会追加
  `(the vhci attach attribute is writable by root only)`，不会只留一句 helper stderr。
- **未做**：本仓没有"导出主机设备"的 `usbip bind` 侧——fork 的方向是**客户端设备进主机**（
  `loopback_usbip_bridge` 作服务端喂 PDU），这条方向与 Windows 完全一致，未改协议。
- **验证**：单测覆盖 argv 约定、差集端口推导、0 基端口表示、无新端口失败、状态文件缺失降级（23 例
  `RemoteUsbHostController.*` 全绿）。真机挂载未验证——本机 `usbip` 未安装、`vhci-hcd` 未加载，
  需要用户 `pacman -S usbip` + `modprobe vhci-hcd` + root 才能做端到端。

### 2.3 GNOME Mutter 显示后端（✅ 已完成，2026-10-22）

`src/platform/linux/mutter_display.{h,cpp}` 是 Mutter 的 `org.gnome.Mutter.DisplayConfig` 客户端，
`display_device.cpp` 在 `niri → mutter → kscreen` 的顺序里分发。与另外两个后端的关键差异都已
按 Mutter 自己的模型落地：

- **写入是整份替换**：`GetCurrentState` 取 serial + 全量配置，改完一次性 `ApplyMonitorsConfig`
  （method 2 = persistent），没有 per-output 动词；因此 `mutter_change()` 统一做
  "读 → 改 → 写 → 回读确认"，3 次重试 / 500 ms。
- **mode id 是字符串**：Mutter 只接受它自己发出去的 id（形如 `2560x1440@144.001`），所以
  `compositor_mode_t::id` 存原串而不是下标；改模式 = 把候选标成 current 后整份写回。
- **禁用输出 = 从 logical monitor 里摘掉连接器**，摘空的 logical monitor 一并删除。
- **主屏是 logical monitor 上的 flag** → `set_as_primary_device` 在 Mutter 上语义与 Windows 一致
  （恰好一个 logical monitor 为 primary），不像 niri 只能退化为"第一个输出"。
- **HDR = monitor 的 color-mode**：1 = BT.2100，0 = default（SDR）；`supported-color-modes` 不含
  BT.2100 时明确报错而不是硬写。
- **复制拓扑**（§1.4）：一个 logical monitor 可含多个 connector，`set_topology` 在 Mutter 会话下
  接受多设备组，`get_current_topology` 用 `replication_source` 聚合回镜像组。

测试：`tests/unit/test_mutter_display.cpp` 用真实 sd-bus 消息做编解码往返（无需合成器，无会话
总线则 skip），7 例全绿；开发中被这批用例抓出 3 个真 bug（根 tuple 不能进容器、monitor 结构
签名少了一层括号、缺可变 `find_monitor` 重载）。

**未在本机运行时验证**：开发机没装 mutter，`busctl --user introspect org.gnome.Mutter.DisplayConfig`
返回 `The name is not activatable`，所以只能靠单测覆盖编解码契约 + 代码走查。**验收**（待有 GNOME
会话时）：客户端"优化游戏设置"能切分辨率/HDR/主屏并还原，镜像拓扑可设置。

### 2.4 DSU 运动数据服务器（✅ 已完成，2026-10-22）

`src/dsu_server.{h,cpp}` 现在是共享源（`cmake/sources/host.cmake`），Windows 的 winsock 专用块
留在 `#if defined(_WIN32)` 内（`WSAIoctl`/`SIO_UDP_CONNRESET`），Linux 走 asio 默认路径。
`src/platform/windows/input.cpp:18` 的 include 改为 `#include "src/dsu_server.h"`，生命周期逻辑
（惰性构造、`start()` 失败即销毁、只告警一次）逐字未动。

Linux 侧在 `src/platform/linux/input/inputtino_gamepad.cpp` 的 `motion()` 里挂载：

- 首个 `LI_MOTION_TYPE_ACCEL`/`LI_MOTION_TYPE_GYRO` 事件到达时才 `init_dsu_server()`，受
  `config::input.enable_dsu_server` 门控（默认 false，WebUI 的"启用 DSU 服务器"+端口 26760
  配置项由共享配置层直接生效）。
- 转发的是**客户端原始值**（加速度 m/s²、角速度 deg/s），与 Windows 一致；两个 motion 类型
  分事件到达，未到达的那一半传 0，服务内部保留上一次的值。
- 服务端把客户端字节序的传感器数据按 cemuhook 的轴向约定重排（X=−x、Y=z、Z=−y、Yaw=−y），
  这部分是 cemuhook 协议本身规定的，与平台无关。

原 gap 文档的"路径修正"判断是错的：DSU 的数据源不是宿主 IMU，而是 Moonlight 客户端经
`gamepad_motion_t` 发来的传感器流（Windows 侧 `src/platform/windows/input.cpp` 的
`dsu_send_motion` 同样如此）。因此不需要 §2.7 的 hidraw 采集先就位，只要客户端是 DualSense
手柄（Linux 的 inputtino PS5 虚拟手柄已经是），运动数据就能到达模拟器。

**测试**：`tests/unit/test_dsu_server.cpp` 用真实回环 UDP 驱动，9 个用例覆盖 INFO 响应
（32 字节、slot_state/device_model/connection_type、MAC 非零）、CRC32 自校验、未订阅时无数据、
订阅后收到 100 字节数据包、加速度/角速度轴向映射、accel/gyro 分次到达时另一半保留、未知消息
类型不回复且服务存活、stop 后同端口可重启。

**一个需要记住的既有行为**：服务端从请求**偏移 8**（header 的 crc 字段）取"客户端 ID"并在响应
里回显，而不是 cemuhook 文档的偏移 12。客户端一律填 0，所以从未暴露；测试按实际行为断言并
在注释里写明，没有改动共享代码的字节布局。

**验收**：Cemu/Ryujinx 在 Sunshine 串流里识别到运动输入（需客户端用手柄串流且开启该选项）。

### 2.5 非 KDE 前台检测（✅ 已完成，2026-10-22）

`src/platform/linux/foreground_app.cpp` 的 producer 选择现在是四级，按能力从强到弱依次探测：

1. **KDE Plasma** — KWin 脚本 + 私有 D-Bus 服务（原有路径，唯一带 pid 的 Wayland 源）。
   只要 `org.kde.KWin` 占着总线名就优先用它，X11 版 Plasma 同样走这条。
2. **wlroots 系（sway / river / wayfire …）** — `wlr-foreign-toplevel-management-unstable-v1`。
   这是**唯一会广播"哪个 toplevel 处于 activated"的通用协议**，所以是 Wayland 的通用解：
   `state` 事件里的 `ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED` 直接给出前台窗口，
   每次 `done` 扫一遍活动项。协议不带 pid，`store_report(0, …)` 靠 exe 名比对照样能识别切换。
   协议 XML 本来就在 `third-party/wlr-protocols/unstable/`（`wlgrab` 用的是同目录的
   export-dmabuf），新增一行 `GEN_WAYLAND` 即可，没有引入新依赖。
   **没有用 wayland-protocols 的 `ext-foreign-toplevel-list`**：它只枚举 toplevel、不带任何
   激活状态，回答不了"谁在前台"这个问题。
3. **niri** — 轮询 `niri msg --json focused-window`（原有路径）。niri 既不实现 KWin 脚本也不实现
   wlroots 的 toplevel 协议，只能走自己的 IPC。查询本身作为探针，避免装了没跑时抢走其它 producer。
4. **X11** — `_NET_ACTIVE_WINDOW` + `PropertyNotify`（根窗口 `PropertyChangeMask`），另有 2s 定时
   重读兜底。字段取自 `_NET_WM_PID`、`WM_CLASS`（取第二段 class，对齐 KWin 的 resourceClass）、
   `_NET_WM_NAME`（`UTF8_STRING`，缺失时退 `WM_NAME`）。X11 库只在 `SUNSHINE_BUILD_X11` 时才链接，
   所以整块都在该宏内。这条同时覆盖"Wayland 合成器完全没有焦点接口时的 XWayland 客户端"。

探测顺序把 KDE 放最前是因为它是唯一带 pid 的源，且 KWin 不实现 wlr 协议——实测当前 Plasma 会话的
registry 里 `zwlr_foreign_toplevel_manager_v1` 与 `ext_foreign_toplevel_list_v1` **都不存在**，
所以通用探针在 KDE 上会快速失败、不会误抢。

**验证**：

- X11 读取逻辑对着自建窗口做过端到端验证（`_NET_WM_PID` / `WM_CLASS` 取 class / 含中文与破折号的
  `_NET_WM_NAME` 三项全部匹配），并已还原被测的根窗口属性。
- wlroots producer 无法在本机运行时验证：当前只有 Plasma 与 niri，niri 不支持该协议，机器上也没有
  sway/river。编译期验证到符号级（`nm` 可见 18 个 `zwlr_foreign_toplevel_*` 符号，生成代码已链接）。
- 仍需在 sway 或 Hyprland 会话上确认 ABR 日志出现前台切换（Hyprland 实现的是
  `ext-foreign-toplevel-list` + 自有的 hyprctl，本 producer 对它同样不生效——Hyprland 目前仍需
  单独的 hyprctl 路径，见 §三 的说明）。

### 2.6 AI Key libsecret 后端（✅ 已完成，2026-10-22）

- 已随 §1.6 一并落地，此处不再列为待办。

### 2.7 ⏸ DS5 本地触觉播放 sidecar——已评估，不移植（2026-10-22）

- **结论**：不移植。它是"主机直连手柄时的高保真触觉回放"增强，不在串流核心链路上，且**本机没有
  任何 DualSense 可验收**。
- **证据（本次复核）**：
  - 硬件：`lsusb` 无 Sony（`054c`）设备；`/sys/class/hidraw/*` 只有 `ITE Device(8910)`、
    `Compx VGN Mouse 2.4G Receiver` ×3、`SYNA2BA6:00 06CB:CE2D`（逐条取自
    `/sys/class/hidraw/hidrawN/device/uevent` 的 `HID_NAME`）。
  - 上游侧车是 Windows 工具链：`scripts/build-ds5-sidecar.ps1` + 完整 .NET 运行时 ZIP +
    `tools/hidmaestro-core-patch/`（HIDMaestro 打补丁 + 注册表隔离）。Linux 侧要重写的是
    `/dev/hidraw` 直写 + 触觉报告编解码，且基础 rumble 已由 inputtino 覆盖。
- **重启判据**：有一台能被 Linux 直连（USB 或蓝牙）的 DualSense 挂在本机 —— 那时才谈得上
  "波形与 Windows 一致"的验收。


### 2.8 ⏸ NVENC SDK 直连——已评估，搁置（2026-10-22）

- **结论**：搁置。avcodec 路径的功能面已齐，直连只剩下精细码控类增益。
- **证据（本次复核）**：`src/video.cpp` 的 nvenc 条目带 `YUV444_SUPPORT` 与
  `AV_PIX_FMT_YUV444P/P16`（`:1441`、`:1457`），HDR 条目带 `CBR_WITH_VBR` 等（`:1649`），
  HDR10+/DV/Vivid 已在 §6.10、§6.20 那几节有实测记录；直连的剩余增益是 fork 的细粒度码控、
  lookahead、多硬件实例。而 fork 的 Vivid T.35 手写序列化器对直连反而是负资产。
- **重启判据**：出现实测的画质/延迟瓶颈，或需要多编码实例（`nvenc_max_sessions`）时再评估。


### 2.9 ⏸ Tauri 控制面板选择性移植——已评估，搁置（2026-10-22）

- **结论**：整体搁置，按需立项。**工具链不是阻塞点**，模块集才是。
- **证据（本次复核）**：本机已有 `webkit2gtk-4.1`（Tauri 在 Linux 上的依赖之一），所以"跑不起来"
  不是理由；真正的阻塞是面板约 20 个系统模块只有 Windows 实现（vdd / vigem / vmouse / rtss /
  hwinfo(WMI) / elevation / 注册表自启 / dualsense elevated / shell 右键菜单），而 WebUI 已覆盖
  配置、配对与大部分监控。
- **重启判据**：用户提出 WebUI 覆盖不到的**具体**需求（那时单独立项，优先 QR 配对、实时监控
  （`nvidia-smi`/PipeWire 统计）、文件右键共享（KDE ServiceMenu 更自然），而不是整体移植面板）。


### 2.10 ✅ 通用 wlroots / X11 显示后端（低）— 2026-10-22 全部完成

- **背景**：目标是 **KDE + niri** 双环境。niri 那半边已落地（见下），只剩 sway/Hyprland 等
  wlroots 合成器的通用兜底没有后端。
- **已完成（2026-10-22）**：niri 输出后端——`src/platform/linux/niri_ipc.{h,cpp}` 做会话判定、
  socket 发现（`NIRI_SOCKET` / `/run/user/<uid>/niri.*.sock` 探活）、20 s 失败冷却与有界执行；
  `src/platform/linux/display_device.cpp` 的 `query_outputs()` 分成 `query_kscreen_outputs()` /
  `query_niri_outputs()` 两个生产者，模式/拓扑开关按会话分派到 `niri msg output <name>
  mode|on|off`。`niri msg` 无论请求是否被接受都 exit 0，所以写操作一律回读状态判定。
- **已完成（2026-10-22，第二轮）：X11（RandR）显示后端** ——
  `src/platform/linux/xrandr_display.{h,cpp}`（`session_supports_xrandr` / `query_state` /
  纯函数 `plan_for` / `apply`，apply 自带快照回滚）+ `display_device.cpp` 的
  `compositor_is_xrandr()` / `query_xrandr_outputs()` / `xrandr_change()`。
  裸 X11 会话（i3/openbox/bspwm…）因此有了分辨率、开关、主屏与**镜像**（两个 output 共用一个
  crtc）；`compositor_is_xrandr()` 排在 niri / Mutter / kscreen 之后，Wayland 会话不受影响。
  HDR 在 X11 上不存在色彩模式概念 → `set_hdr_states()` 明确拒绝并打一次性 warning。
  实测 `query_state()` 与本机 `xrandr --query` 逐字段一致；写路径在 XWayland 上无法真正生效
  （其 RandR 只读），真机 X11 会话未验证。详见 `LINUX_MIGRATION_REPORT.md` §6.21。
- **已完成（2026-10-22，第三轮）：wlroots（wlr-randr）显示后端** ——
  `src/platform/linux/wlr_randr.{h,cpp}`：`wlr-randr` 的列表解析（纯函数 `parse_state`）、
  `--mode` 规格与 millihertz 的往返（`mode_spec`）、请求到 argv 的构造（纯函数 `build_args`，
  输出名按 shell 规则转义）、有界执行（`run` 复用 `vdd_utils::run_logged`）以及"探测缓存 + 20 s
  冷却"的会话判定（`session_supports_wlr_randr`）。`display_device.cpp` 侧新增
  `compositor_is_wlr_randr()` / `query_wlr_randr_outputs()` / `wlr_randr_change()`，接入开关、
  模式、拓扑；主屏与 HDR 按协议能力显式跳过；镜像（一个 timing 两个 head）协议不支持 →
  `compositor_supports_replication()` 返回 false。
  **判定顺序**：niri → Mutter → wlroots → X11(RandR) → KScreen。wlroots 排在 RandR 之前是有意的：
  wlroots 会话里 XWayland 也会应答 RandR 读取，而真正拥有连接器的是 wlroots 的输出管理器。
  `--dryrun`（协议的 `test` 请求）已打通，但写路径目前只做"回读校验"，与另外三个后端一致。
  验证：解析/构造/millihertz 往返共 8 个单测（`tests/unit/test_wlr_randr.cpp`）；**用真源码在
  /tmp 编译出 wlr-randr**，在本机 KDE 会话下实测其失败分类（"compositor doesn't support
  wlr-output-management-unstable-v1"、exit 1；无合成器时 "failed to connect to display"）正是
  本后端的探测依据。本机没有任何 wlroots 合成器，**成功路径（读回真实 head、真正改模式）无法
  运行时验证**。详见 `LINUX_MIGRATION_REPORT.md` §6.22。
- **仍为已知边界**（非缺口，属合成器/协议能力）：
  1. **niri 无 HDR 控制**：IPC 不暴露 HDR 亮度/开关，`hdr_state_str` 恒空 → 客户端 HDR 请求被
     记录并跳过（`compositor_is_niri()` 分支），不会失败。niri 侧需上游先提供接口。
  2. **niri 与 wlroots 无主屏概念**：`output <name> priority` 一类命令/wlroots 协议字段都不存在，
     `ensure_primary` 请求被跳过并打一次性 warning。
  3. **缩放/位置未纳入**：`niri msg output <name> scale|position` 与 `wlr-randr --pos/--scale` 都可用，
     但 display_device 的拓扑模型只有"哪个输出开着"，没有布局坐标（Windows 侧也是 per-device 模式，
     不写布局）。
  4. **wlroots 无 HDR、无镜像**：协议里都没有对应字段/动词。
- **验收**：niri 会话下客户端分辨率请求能作用到目标输出（已在嵌套 niri 26.04 上验证 CLI 语法与
  JSON 结构；真实显示器上的端到端验证需要 niri 会话）。
- **注意**：一律先做能力探测，探测失败保持既有降级路径（不得影响 KDE 与非 KDE 现状）；单次探测
  有 1.5 s 上界，失败后有 20 s 冷却，niri 会话里由 `NIRI_SOCKET` 直接命中、不发探针。

### 2.11 ⏸ VAAPI 的 YUV 4:4:4——已评估，不启用（2026-10-22）

- **结论**：不启用。代码链路已备但走不到，且**本机驱动根本不提供编码入口**。
- **证据（本次复核）**：
  - 本机 VAAPI 驱动是 `/usr/lib/dri/nvidia_drv_video.so`（`libva-nvidia-driver`），**只实现解码**：
    `nm -D` 只导出 `__vaDriverInit_1_0`，`strings` 里 `NvEncodeAPICreateInstance|nvEncode|NvEncoder`
    命中 **0** 次 ⇒ 它不会暴露任何编码 entrypoint。
  - `src/video.cpp:1764-1770` 的 vaapi 条目第三对格式仍是 `AV_PIX_FMT_NONE, AV_PIX_FMT_NONE`
    （无 444），与文档记录一致；`get_va_profile()` 虽能把 `HEVC_REXT` 映射到 `Main444/Main444_10`，
    但探测会因驱动不支持而自然拒绝。
- **重启判据**：本机换成 Intel/AMD iGPU（或出现带编码的 VAAPI 驱动），且确有 HEVC 4:4:4 的保真
  需求；那时按 §6 那段做法补 444 格式 + 把 `va_t::set_frame()` 的 surface 导出从 2 层泛化到
  3 平面。


## 三、⛔ 本质不可移植（含原因与 Linux 对应物）

| Windows 功能 | 不可移植原因 | Linux 对应物 / 状态 |
|---|---|---|
| Zako Direct 零拷贝借帧 | D3D11 共享纹理，无 Linux 对等机制 | KMS 捕获直读 DRM plane（功能面已覆盖） |
| WGC 捕获 | Windows Graphics Capture API | kmsgrab ✅ |
| rtx_hdr / TrueHDR 链 | RTX Video HDR = NVIDIA Windows 驱动 API + D3D11 合成管线 | ⛔ 无；且 `rtsp.cpp` 的 TrueHDR PQ 链路整体 Windows 门控 |
| AMF 硬件编码 | AMD Windows 运行时 | VAAPI（上游已有；N 卡不适用） |
| HLG 捕获源 → DV P8.4 | 内核 `HDR_OUTPUT_METADATA` UAPI 只定义 PQ/SDR | ⛔ 等内核演进；P8.1 已覆盖 DV 主流 |
| ZakoVDD 硬件光标导出通道 | Windows 驱动私有通道 | KMS 光标平面（合成器渲染光标进捕获）✅ |
| 触摸键盘自动唤起 | Windows 任务栏注册表机制 | 头文件本身非 Windows 即 no-op，无需处理 |
| nvprefs 驱动设置 | NVIDIA Windows 控制面板 API | nvidia-drm modeset 参数（系统层已配置）✅ |
| 服务包装器 / UAC 提权 / Win 深色模式 / GFE 兼容 | Windows 平台概念 | systemd 用户服务 ✅ / 无此概念 ✅ |

---

## 四、有意保留的差异（决策记录，非缺口）

以下项经复核判定**保持现状**，理由记录在此，便于日后按需推翻：

- **空容器契约**：Linux 的 `get_current_*` 以"空集合 = 全部已启用输出"、setter 以"空 map = 无需处理
  （true）"为准；Windows 分别返回 `{}`/false。树内**没有调用方**传空集合查询，而 setter 的空 map
  语义被还原路径依赖（"没有要还原的" → 不能算失败）。改成 Windows 语义只会把无操作变成失败。
- **麦克风契约边界**：null samples → 0（Windows -1）、重复 init → 0（Windows -1）、返回字节数 2 B/帧
  （Windows 端点相关 4 B）、缓冲属性留服务端默认（Windows 显式 100 ms）。消费者 `stream.cpp` 只区分
  `==0`（丢帧计数）与 `<0`（重初始化）；改成 -1 反而会触发无意义的重初始化循环。
- **日志语言**：同一事件 Windows 中文 / Linux 英文。Linux 端整个日志面是英文，混语种更差；**面向用户
  的 UI 文案走托盘 i18n（中/英/日）**。日志字符串不参与行为或线协议契约。
- **"blank HDR toggle"**：Windows 在切换 HDR 前把新启用显示器先切到相反状态、等 2333 ms 再设最终值，
  是 Windows 显示栈（IDD/VDD）的"颜色发白"清理手段；Linux 的 VDD 是真实 DRM 连接器、无对应症状，
  移植只会给每次新启用显示器加 2.3 s 延迟。如实测出现同症状再补。
- **前台 exe 语义**：Linux 报 Wayland app class、Windows 报进程映像名（含 `.exe`）。Wayland 下没有
  等价的"进程映像名"来源（合成器只暴露 app_id/class），改成"猜进程名"会引入不可靠映射。
- **直方图估计器**：Linux 精确 1024 码直方图 vs Windows 256 bin 采样——语义一致、数值不同，Linux 更
  精确。
- **剪贴板线协议常量的跨语言同步**：C++ 侧已收敛到 `src/clipboard_bridge.h`（版本/kind/帧头/内联阈值/
  TTL），Rust agent（`clipboard.rs`）是权威参考且无法包含 C++ 头文件。为 5 个协议常量引入代码生成
  步骤不划算；C++ 镜像由 `tests/unit/test_clipboard_wire.cpp` 的编解码测试守住。
- **显示设置失败时的返回值**：合成器不可用时 display_device 保留 success 结果（Linux 上这是会话的持久
  属性，返回失败会让所有非 KDE 会话进入无法修复的 deferred-retry），但日志提到 warning 并明确
  "显示设置未被应用、串流按当前布局继续"。

---

## 五、上游新特性在 Linux 的缺口

上游 `ed415026`（并入本分支的 50 个提交）带来的新特性在 Linux 侧的现状。这些**不是 fork 的 Windows
增强**，而是上游自己的新功能。均已确认**不会破坏 Linux 构建**（共享层能编译，平台后端缺失处以
`#ifdef _WIN32` 门控或探测拒绝兜底）。

### 5.1 ○ PyroWave 编码后端仅 Windows（高）

- **现状**：共享层完整编译——`src/pyrowave/{types,config,capabilities,gpu_interop,packet,packetizer,
  dynamic_hdr,session,runtime}.cpp` 都在 `cmake/compile_definitions/common.cmake`，
  `tests/unit/test_pyrowave.cpp` 在 Linux 跑通。缺的是**平台后端**：
  `src/platform/windows/pyrowave/{cpu_encoder,gpu_encoder,transport}.cpp` 只在 `windows.cmake`；
  `video.cpp` 的 PyroWave 编码派发整块位于 `#ifdef _WIN32`。
- **Linux 行为**：本分支从未把 PyroWave 编码器注册进编码器表，客户端不会协商到它；即便协商到，
  `encode()` 的派发链会落到末尾 `return -1`，会话按既有的"PyroWave 会话初始化失败 → shutdown"
  路径结束，不会静默出坏流。
- **若要做**：Linux 需要 CPU 后端（x264/x265 软件编码 + 块级封装，`cpu_encoder.cpp` 无 D3D 依赖，
  是最可行的一块）与传输层（`transport.cpp` 的 socket/重建路径）。GPU 互操作（`gpu_interop.cpp`）
  在 Linux 需对应 CUDA/EGL 纹理导入。
- **工作量**：中-高（CPU 后端中）。**验收**：Linux 客户端协商 PyroWave 后能出画且 RS 重传生效。

### 5.2 ○ 动态 RS-FEC 已随共享层落地（已基本完成）

- **现状**：`src/streaming/fec.cpp`（动态 RS 保护 + 客户端覆盖）与 `src/streaming/bitrate.cpp`
  （会话码率预算，含音频/开销扣除与 FEC 折算）都在共享列表里，Linux 与 Windows 同源编译。
  `video.cpp::set_bitrate` 已对齐该预算模型——**Linux 的 FFmpeg avcodec 动态码率因此走统一的预算链路**。
- **剩余**：`perf_recorder`、`widget_http` 同样在共享列表（Linux 可用）。真正平台相关的是 NVENC 原生
  帧预算守卫（§5.3）。
- **验收**：Linux 客户端改码率时日志出现 `Applied video budget: total=... encoder=... FEC=...`。

### 5.3 ○ NVENC 帧预算守卫对 Linux 无意义（决策）

- **现状**：`src/nvenc/frame_budget.{h,cpp}` 是**纯函数**（无 SDK、无硬件访问），已加入
  `PLATFORM_TARGET_FILES` 以满足共享 `confighttp.cpp` 的链接需求。但**发布者**
  `publish_frame_budget_report()` 的调用点在 `src/nvenc/common_impl/nvenc_base.cpp`，而整个
  `src/nvenc/` 原生 SDK 路径（含 `common_impl/`）只在 Windows 编译。
- **Linux 行为**：`GET /api/config` 不含 `active_nvenc_frame_budget` 块——Linux 的 NVENC 是 FFmpeg
  的 `*_nvenc` 封装，不经过原生 SDK 的 `create_encoder()`，没有"配置预设 vs 有效预设"的概念。
  这是**正确行为**，不是缺口。
- **验收**：Linux `/api/config` 不出现该块，且不报错。

### 5.4 ⛔ DLSS NR / RTX HDR / Game Bar 小部件等（Windows 专有 SDK）

- **现状**：`sunshine_dlssnr_adapter` / `sunshine_rtx_video_adapter`
  （`cmake/dependencies/dlssnr_adapter.cmake`、`rtx_video_adapter.cmake`）与
  `src/platform/windows/image_enhancement/**` 全部 Windows-only；`src/widget_http.cpp` 共享层在 Linux
  编译，但其 UWP 端点骨架是 Windows 概念。
- **Linux 对应物**：图像增强在 Linux 无等价物（CUDA/Vulkan 自研滤波属独立大特性）；Game Bar 小部件与
  Windows 外壳绑定，无移植意义。

### 5.5 ○ "下线显示模式重映射"重构（已完成，无需跟进）

- **现状**：上游 `8b905e2c` 删除了 `src/display_device/parsed_config.cpp` 的
  `remap_display_modes_if_needed()` 与配置键 `display_mode_remapping`。已核实全树无残留引用，
  Linux 的 `explicit_vdd` 分支（`ZAKO_DEVICE_ID`/`ZAKO_NAME`）完好保留。
- **验收**：Linux 客户端指定虚拟屏仍走 VDD 目标。

---

## 六、仍未完成（诚实清单）

1. **HLG 域分析源**：Linux 的分析器只按 PQ 解释像素，因此 HLG 会话没有 HDR Vivid、DV P8.4 也被门控
   拒绝。补齐需要新的分析源（预编码线性域，或 shader/readback 的 HLG 域映射），属独立特性；
   本机（PQ + NVIDIA）也无法端到端验证。
2. **niri 输出后端**（§2.10）：**已完成**（2026-10-22）。niri IPC 后端落地，模式/拓扑开关在
   niri 会话下生效；HDR 与主屏因 niri 无对应接口而显式跳过（见 §2.10"仍缺"）。剩下的是
   sway/Hyprland 的 wlr-output-management 通用兜底。
3. **D15 复制拓扑**（§1.4）：kscreen-doctor 无 replication 命令（已实测确认），KDE 上实现镜像需 KWin
   脚本或 libkscreen 后端，属独立特性。
4. **§四的有意保留差异**：如需翻转，按各条给出的理由逐项决策即可。
5. **DisplayID 扩展块（中，可选）**：> 655.35 MHz 的模式（3440x1440@120/144、3840x2160@90/120/144）
   目前只能明确拒绝。要真正支持，需要在 EDID 里追加 DisplayID 扩展块（24 位像素时钟），并让 base
   block 的首选 DTD 退回一个可行模式、由会话显式切到该模式；成败取决于内核 DRM 与 KWin 是否把
   DisplayID timing 纳入模式表，需实机验证。
6. **合成器未重探测 EDID 改写的可能**（低）：若实机出现"模式可行但始终未生效"，即为此因。

---

## 七、2026-10-22 上游同步新引入的缺口

同步点：`ed415026` → `7a0995e2`（33 个提交，403 文件，+29538/−12656）。本节只记录**本次同步带来
的新缺口**；连同 §一/§二 一起才构成完整待办。

### 7.1 ✅ 管线帧率的"源更新"段（KMS 路径已接上，2026-10-22）

**上游机制**：`src/streaming/perf_recorder.{cpp,h}` + `src/streaming/frame_rate_tracker.h`
（`capture_source_kind_e` 与 `capture_source_counter_t`）。平台钩子是
`platf::display_t::frame_rate_source()`（`src/platform/common.h:685`，默认返回空指针），
Windows 三个后端 override 它（`display.h`、`display_vdd.cpp:1453`、`display_wgc.cpp:447`）。

**已做**：`src/platform/linux/kmsgrab.cpp` 的 `display_t` 现在也 override：

- 新增枚举值 `capture_source_kind_e::drm_fb_flips`（JSON 里就是 `"drm_fb_flips"`，映射在
  `src/streaming/perf_recorder.cpp:300`）。这是纯增量，Windows 侧不受影响（switch 有 `default`）。
- `refresh()` 每次读到 `plane->fb_id` 后调用 `observe_scanout(fb_id)`：**扫描出的帧缓冲变了**
  才记一帧。语义与 DXGI present / WGC arrival 一致——静态桌面记 0，这才是这个指标要抓的
  "上游没出新帧"场景。注意 kmsgrab 的捕获循环是**按串流帧率定速**的：屏幕冻住时 `capture` 段
  照样报 60 fps，所以源段和捕获段不是同一个数，接上它是有意义的。
- 驱动的 vblank 计数器两条路都试过，**NVIDIA 全不支持**（实测 `errno=95 EOPNOTSUPP`）：
  `drmCrtcGetSequence`（`/dev/dri/card1` 上直接返回 −1）、`drmWaitVBlank`
  （`DRM_VBLANK_RELATIVE` 同 errno）。所以没有把"绝对刷新率"当源速率——那在主力卡上恒为空。
- 无权限时 `drmModeGetPlane` 会返回 `fb_id=0`，此时直接跳过，不产生错误的 0 帧；有权限时它读的
  就是捕获本身依赖的那个值（`fb_id` 为 0 时 `refresh()` 本来就会 `timeout`），所以"能被这个
  生产者看到"的前提与"能捕获"完全等价。

**另外两个 Linux 后端为什么不接**：

- `wlgrab.cpp`（`wlr-export-dmabuf`）：`dmabuf_t::listen()` 是**一帧一请求**，捕获循环每轮发一次
  请求等一帧，源速率与捕获速率恒等，单独记一列只会误导。合成器侧的 `wp_presentation` 反馈需要
  我们自己持有 `wl_surface`，screencopy 客户端拿不到。
- `x11grab.cpp`：定时 `XShmGetImage` 轮询，没有 damage 事件接入（未用 XDamage），同样没有独立
  信号源。

**验收**：`tests/unit/test_perf_frame_rates.cpp` 的 `ReportsDrmFbFlipsAsTheSourceKind` 覆盖
枚举→JSON 映射与"144 次翻转 / 60 次捕获"的分离；真实 KMS 会话下 `/api/perf/current` 的
`source_kind` 应为 `drm_fb_flips`（需要带能力的二进制 + 活动会话，headless 环境无法复现）。

### 7.2 ✅ 依赖外网的单元测试（上游已自行摘出 ctest 门禁，2026-10-22 复核）

- `DownloadFileTests/DownloadFileTest.Run/1` 访问
  `https://httpbin.org/redirect-to?url=/base64/aGVsbG8h`。离线机器必失败，与本仓代码无关。
- **上游已经把这条路从 ctest 门禁里摘掉了**：`tests/CMakeLists.txt:613-618` 注册的
  `test_sunshine` 用例带 `--gtest_filter=-DownloadFileTests/*`，注释写明该参数组 "report on a
  third party's uptime rather than on this repository"，用例留在二进制里，改
  `http::download_file` 时可手动跑。
- 因此 `ctest` 不会因它变红，只有直接跑二进制才看得到失败。**无需动作**，更不要改产品代码。

### 7.3 ✅ 依赖系统已安装资产的单元测试（2026-10-22 修复）

- `tests/unit/test_crypto.cpp` 的
  `NvhttpApps.StreamsLargeAssetOverTlsAndPreservesNotFoundAndBadArguments` 断言"未知 appid →
  404"。但 `DEFAULT_APP_IMAGE_PATH` 是 `SUNSHINE_ASSETS_DIR "/box.png"`（`src/process.cpp:52`），
  编译期就定成本机安装前缀的绝对路径 `/usr/share/sunshine/box.png`；本机装过包，该文件存在 →
  实际返回 200 并把 box.png 流出。（曾用用户命名空间 `unshare -rm` 把空目录 bind-mount 到
  `/usr/share/sunshine` 验证：该断言是环境假设，不是移植回归。）
- **已修**：用例先探一次编译期默认资产是否存在（`fs::exists(fs::path(SUNSHINE_ASSETS_DIR) /
  "box.png")`），据此决定未知 appid 期望 200 还是 404；同时把真正要守的性质补强为"响应**永远不是**
  请求里那个 app 自己的图"——期望 200 时断言 body 长度小于请求的 65 MiB 资产。两种环境都通过，
  断言没有被削弱成恒真（`tests/unit/test_crypto.cpp:376-395`）。
- 结果：`test_sunshine` 805 例 / 792 通过 / 12 跳过 / 1 失败，剩下的就是 §7.2 那条被 ctest
  过滤掉的 httpbin 用例。

### 7.4 三个 SetUpTestSuite 失败（AudioTest / MouseHIDTest / EncoderTest）——权限，不是缺口

这 3 个失败从移植第一天起就在（`AGENTS.md` 的构建基线亦记为 headless 现象），本轮查清了根因：

- 报错点在 `tests/tests_common.h:18` 与 `:23` 的 `ASSERT_TRUE(platf_deinit)`：`platf::init()`
  返回 `nullptr`（`src/platform/linux/misc.cpp:1092-1095`，没有任何采集后端自检通过）。用例体
  本身是优雅跳过的（如 `tests/unit/test_audio.cpp:56`），只有夹具断言报错。
- 根因是**能力**：KMS 自检走到 `src/platform/linux/kmsgrab.cpp:1837` 的
  `if (!fb->handles[0])`——取 DRM framebuffer 的 GEM handle 需要 `cap_sys_admin`。已核对本机
  `/usr/bin/sunshine-*` 带 `cap_dac_override,cap_dac_read_search,cap_sys_ptrace,cap_sys_admin=p`
  （`packaging/arch-local/sunshine-foundation.install` 的 `do_setcap()` 打上），而
  `build/tests/test_sunshine` **没有能力** → 该分支 `break` → `display_names` 为空 → init 失败。
- 设备权限不是原因：`/dev/dri/card1`、`/dev/dri/renderD128`、`/dev/uinput` 在当前用户下都能
  `O_RDWR` 打开（已实测）；`strace` 也确认测试进程确实打开了 `card1` 与 `renderD128`，之后才因
  拿不到 fb handle 放弃。
- **结论**：给测试二进制加上能力即会通过。不要改动上游夹具把它变成 skip——那会在 Windows CI 上
  掩盖真实的 `platf::init()` 失败。`ctest` 的 `test_sunshine` 因此仍报红，属预期。

### 7.5 已随本次同步自动继承（无需后续动作，记录以免误判为缺口）

- **HTTP 传输替换**：Simple-Web-Server → 自带 Beast 实现（`src/http/{types,server,client,
  tls_trust}.{h,cpp}`，`cmake/targets/beast_http.cmake` 建 `sunshine_http` 静态库）。本仓 24 个
  include 点、AUR PKGBUILD 的 `source=`/`prepare()`、`third-party/Simple-Web-Server` 子模块与
  工作区残留目录已全部清掉。`web::StatusCode` 的枚举名与旧 SimpleWeb 不同（`not_found` 等），
  本仓 Linux-only 文件已随合并改到位。
- **confighttp 拆分**：`src/confighttp.cpp` 4270 行 → 171 行，逻辑进 `src/confighttp/` 24 个文件；
  `saveVddSettings` 上移为 `display_device::vdd_settings::save()`（`src/display_device/vdd_settings.cpp`，
  已在 `cmake/sources/host.cmake:32`，Linux 也编）。
- **去门控重新打回**：`/api/vdd/status`（`src/confighttp/diagnostics.cpp:420-422`）与
  `/api/runtime/hdr` 的 `available`（`src/confighttp/runtime.cpp:121`）上游仍是 `#ifdef _WIN32`，
  本仓保留跨平台版本（冲突已手工解决）。
- **text_context 门控**：`notify_gui_alive` 扩到 6 参数（input_pane/uia/agent_present/observer_active/
  generation/agent_id）。Linux 无 GUI agent 上报，`gui_alive()` 恒 false，行为不变。
- **笔 barrel roll**：共享层（`src/pen_barrel_roll.h`、`pen_input_t::barrelRoll`）到位；inputtino 的
  `place_tool()` 没有 barrel roll 参数，Linux 平台端无处落地，`src/rtsp.cpp` 的
  `x-ss-general.penBarrelRoll` 广告也仍被 `#ifdef _WIN32` 挡住——客户端不会协商到，行为正确。
- **AMF 两个提交**（AMF 积压输出、AMD HEVC GOP）：`src/amf/*` 只在
  `cmake/compile_definitions/windows.cmake:118-137` 编译；新增的 `amd_hevc_gop_size` 配置键走共享层。
- **仓库根整理**：`assets/brand/`、`assets/installer/`、`scripts/vite/`、`docs/{readme,en,zh-CN}/`
  已就位；`cmake/packaging/linux.cmake` 自动指向 `assets/brand/sunshine.svg`，本仓额外安装的
  `logo-sunshine-*.png` 规则保留。

### 7.6 ✅ 显示后端判定顺序（工程质量，2026-10-22）

五条显示后端（kscreen / niri / Mutter / wlroots / RandR）里，有两条会在**不属于它**的会话里应答：
XWayland 在每一个 Wayland 会话里都应答 RandR 读，KScreen 只要 `kscreen-doctor` 存在就应答。因此
"先问谁"本身就是契约，`display_device.cpp` 里四个匿名 namespace 的 `compositor_is_*()` 助手
却把它藏在调用图里，无法单测。

**已抽成 `src/platform/linux/display_backend.h`**：header-only 的
`platf::select_display_backend(supported)` 按 `niri → mutter → wlr_randr → xrandr → kscreen`
依次问谓词，第一个举手即答案、之后的谓词一次都不调用（探测不白花；wlroots 的探测可能真的去跑
一次 `wlr-randr`）。`compositor_is_*()` 与 `query_outputs()` 的派发都改为经由
`selected_backend()`，逐次探测成本与改前完全相同；顺序理由逐条写在头文件里。
`tests/unit/test_display_backend.cpp` 7 例用假谓词钉死顺序（含"wlroots 必须赢过 XWayland 镜像"
与"探测在胜者处停止"），无需任何合成器。
