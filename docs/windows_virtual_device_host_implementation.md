# Windows Sunshine 虚拟设备宿主实施方案

## 1. 文档状态

- 状态：实施设计稿
- 日期：2026-08-28
- 目标平台：Windows 10/11 x64；ARM64 在首期完成接口设计但不承诺发布
- 进程：Sunshine Core + 一个 Core 持有的虚拟设备宿主
- 初期运行时：HIDMaestro 发布标签 `v1.6.2`（`HIDMaestro.Core.dll` 的 `AssemblyFileVersion = 1.6.2.0`）+ 未修改的已签名 usbip-win2 0.9.7.7
- 首个新增设备：`Sunshine Virtual Microphone`

本文把现有 `Sunshine.Ds5Sidecar` 从“虚拟 DualSense 的附属进程”演进为“Sunshine 虚拟设备宿主”。宿主继续隔离第三方 .NET 运行时，但同时管理彼此独立的虚拟 DS5、DS5 USB Audio 和 USB 虚拟麦克风。

本文覆盖进程所有权、协议扩展、设备生命周期、麦克风数据面、代码落点、测试和发布闸门。它补充 `docs/windows_dualsense_component_lifecycle.md`；若两者对 Sidecar 进程生命周期的描述冲突，以本文的宿主模型为后续实现目标，现有 DS5 设备本身仍保持按会话管理。

## 2. 目标与非目标

### 2.1 目标

1. 一个宿主进程承载所有 HIDMaestro 虚拟设备，不为麦克风再创建第二个常驻进程。
2. DS5 和虚拟麦克风具有独立生命周期；其中一个设备失败不能要求另一个设备一起销毁。
3. 使用用户态标准 USB Audio Class 设备和 Windows inbox USB Audio 驱动，不编写新的 Windows 音频功能驱动。
4. 允许 Sunshine 将客户端麦克风 PCM 直接提交到 USB Audio IN endpoint，绕过 VB-CABLE 的 render/capture cable 路由。
5. 虚拟麦克风在 Moonlight 会话间保持稳定身份，客户端断开时输出静音而不是从 Windows 中消失。
6. 保留 VB-CABLE 后端和明确回退路径，USB/IP 功能首期默认标记为实验性。
7. 控制面可靠优先；高频音频不能阻塞手柄输入、设备 detach、状态回复或宿主关闭。
8. 不修改 usbip-win2 内核驱动，从而保留上游签名和升级边界。

### 2.2 非目标

- 不让宿主脱离 Sunshine Core 成为系统服务或长期孤儿进程。
- 不在首期自动修改 Windows 默认录音设备。
- 不用虚拟麦克风承载 Dolby MAT、Atmos、IEC 61937 或其他压缩 bitstream。
- 不在首期实现 USB Audio render endpoint；独立麦克风 profile 只暴露 capture endpoint。
- 不在首期删除 VB-CABLE 代码或自动卸载已安装的 VB-CABLE。
- 不在首期支持多个 Sunshine 虚拟麦克风实例。
- 不把远程 USB/IP 暴露到网络；USB/IP device server 只允许本机回环使用。
- 不在首期重写 HIDMaestro 的 USB/IP device framework，也不在 Sunshine 中宿主 CLR。

## 3. 当前实现基线

### 3.1 DS5 Sidecar

当前 `tools/sunshine-ds5-sidecar` 是 .NET helper：

- 加载经校验的 `HIDMaestro.Core.dll`。
- 使用 `SDS5` v1 二进制 Named Pipe 协议。
- 一个 pipe 连接只接受一个经验证的 elevated Sunshine owner。
- owner EOF、broken pipe 或 Core Job Object 关闭时，Sidecar 销毁该连接创建的全部虚拟设备并退出。
- 普通 DS5 使用 HIDMaestro UMDF2 后端；复合 DS5 使用 USB/IP 后端。
- `dualsense-composite` 已定义 48 kHz、16-bit、双声道 UAC1 麦克风 IN endpoint，但当前 `ControllerSession` 只消费 USB Audio OUT 数据，没有向麦克风调用 `Submit()`。

### 3.2 麦克风重定向

当前 Windows 路径由 `mic_write_wasapi_t`：

1. 寻找或引导安装 VB-CABLE。
2. 打开 VB-CABLE render endpoint。
3. 通过 `IAudioRenderClient` 写入 48 kHz、mono S16 PCM。
4. 将 VB-CABLE capture endpoint 作为应用看到的麦克风。
5. 在会话开始时初始化设备，在最后一个麦克风会话结束后释放并恢复默认端点。

目标实现保留上游 PCM 解码、混音和 `write_mic_pcm()` 调用契约，只替换最终 sink。

## 4. 核心架构决策

### 4.1 一个宿主进程，而不是一个设备一个 Sidecar

```text
Sunshine Core
  |
  | SDS5 v1 Named Pipe（控制 + 有界实时消息）
  v
Sunshine Virtual Device Host
  |-- ControllerSession[0..N]
  |     |-- UMDF2 DS5
  |     `-- USB/IP DS5 composite
  |
  `-- VirtualMicrophoneSession[0..1]
        `-- USB/IP UAC1 capture-only device
              `-- usbip-win2 UDE bus
                    `-- Windows usbaudio.sys
```

首期继续使用 `Sunshine.Ds5Sidecar.exe` 文件名和现有组件 manifest，避免一次改动同时触发下载资产、安装目录、摘要和 GUI 迁移。代码内部先引入通用 `VirtualDeviceHostServer`；待协议和组件升级路径稳定后，再单独迁移到 `Sunshine.DeviceSidecar.exe`。

### 4.2 宿主进程生命周期与设备生命周期分离

宿主进程由 Core 唯一持有，始终放入 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object。宿主不得在 owner pipe 断开后继续运行。

启动规则：

- USB 虚拟麦克风已由用户启用：Core 完成配置加载、组件校验和交互会话检查后启动宿主并创建麦克风。
- 没有启用 USB 麦克风，但需要虚拟 DS5：首个 DS5 分配时按现有方式启动宿主。
- 两种功能同时使用：复用同一个宿主和 owner pipe。

停止规则：

- USB 虚拟麦克风启用期间，Moonlight 会话结束不会停止宿主，也不会销毁麦克风。
- DS5 仍可在最后一个 DS5 会话结束后的宽限期内 detach，不影响麦克风。
- 用户明确关闭 USB 麦克风且不存在其他虚拟设备时，Core 可优雅关闭宿主。
- Core 退出、owner pipe 断开或 Job Object 关闭时，宿主销毁全部设备并退出。

### 4.3 保持 `SDS5` v1 帧格式并以 capability 扩展

不立即修改 16-byte header、magic 或 `Version = 1`。新 Core 只有在 `HelloReply` 包含对应 capability 后才发送麦克风消息：

```text
VirtualMicrophone      = 1 << 10
PersistentDeviceHost   = 1 << 11
MicrophoneStatus       = 1 << 12
```

这样可以保证：

- 旧 Core 面对新宿主仍只使用已有 DS5 消息。
- 新 Core 面对旧 Sidecar 时识别到 capability 缺失，不发送未知消息。
- 用户显式选择 USB 麦克风时返回“组件需要更新”，而不是静默创建错误设备。

协议 magic 和可执行文件命名在后续版本中可以重命名，但不与麦克风第一阶段绑定。

### 4.4 设备故障域独立

以下操作必须只影响目标设备：

- DS5 attach/detach 不创建或销毁虚拟麦克风。
- 麦克风 `flush`、远端断流或 endpoint pin 关闭不 detach DS5。
- 麦克风提交失败先进入该设备的 fault/recreate 流程，不立即退出整个宿主。
- 只有 pipe 协议损坏、owner 身份失效、HMContext 全局故障或宿主 shutdown 才清理全部设备。

usbip-win2 的内核故障无法由用户态隔离；该风险通过版本固定、压测和发布闸门管理。

## 5. 运行状态模型

### 5.1 宿主状态

```text
stopped
  -> starting          Core 创建进程并连接 pipe
  -> negotiating       Hello/capability 协商
  -> ready             可以管理设备
  -> degraded          至少一个设备失败，其他设备仍可用
  -> stopping          Core shutdown 或最后一个功能关闭
  -> stopped

starting/negotiating/ready/degraded
  -> faulted           pipe、身份或 HMContext 全局故障
  -> stopping
```

### 5.2 虚拟麦克风状态

```text
absent
  -> creating          加载 profile、创建 USB/IP 设备
  -> enumerating       等待 Windows capture endpoint 稳定
  -> idle              endpoint 存在，主机应用尚未打开
  -> remote_active     capture pin 未打开但收到有效远端 PCM（is_host_streaming=false）
  -> idle              capture pin 未打开且收到 STREAM_END 或 MicFlush（is_host_streaming=false）
  -> host_capturing    Windows 应用已打开 UAC IN stream
  -> remote_active     有有效客户端 PCM
  -> host_capturing    客户端断流，继续提交静音
  -> idle              主机应用关闭 capture pin
  -> destroying        用户关闭功能或宿主退出
  -> absent

creating/enumerating/idle/host_capturing/remote_active
  -> device_faulted
```

`host_capturing` 和 `remote_active` 是两个正交事实。`state` 表示最重要的活动来源，`is_host_streaming` 独立表示 Windows capture pin 是否打开；Core 和 GUI 必须按下表解释组合，而不能只读取其中一个字段：

| 应用/capture pin | 远端 PCM | `state` | `is_host_streaming` | Core/GUI 解释 |
|---|---|---|---|---|
| 已打开 | 无 | `host_capturing` | `true` | 应用正在采集，宿主提交静音 |
| 已打开 | 有 | `remote_active` | `true` | 远端 PCM 正被主机应用消费 |
| 已关闭 | 仍在发送 | `remote_active` | `false` | 远端仍活动，但没有主机消费者；pin close 会先清空已排队 PCM，后续数据仍受有界队列约束 |
| 已关闭 | 无 | `idle` | `false` | endpoint 已枚举但双方均空闲 |

应用关闭时若队列中仍有 PCM，宿主必须在发出状态前清空队列，因此不会向 Core 暴露“pin 已关闭但保留陈旧 buffered bytes”的稳态。没有远端 PCM 不表示 Windows 应用没有打开麦克风；此时必须输出静音。

### 5.3 生命周期不变量

1. 同一宿主最多存在一个 `Sunshine Virtual Microphone`。
2. Moonlight 会话计数从 1 变为 0 时只执行 `MicFlush`，不执行 `MicDestroy`。
3. 只有配置关闭、显式修复流程或宿主退出可以销毁麦克风。
4. `MicCreate` 是幂等操作；重复请求返回现有 generation 和 format。
5. 每次重新枚举设备 generation 单调递增，旧 generation 的 PCM 必须丢弃。
6. endpoint identity 由固定 serial/container identity 保持，不由 Moonlight session ID 派生。

## 6. 虚拟麦克风 USB Profile

新增：

```text
tools/sunshine-ds5-sidecar/profiles/sunshine-virtual-microphone.json
```

首期固定格式：

| 属性 | 值 |
|---|---|
| USB class | Audio Class 1.0 |
| 功能 | `audioControl` + `audioStreamingIn` |
| endpoint | Isochronous IN |
| 采样率 | 48,000 Hz |
| 声道 | 1 |
| 位深 | 16-bit signed little-endian PCM |
| packet interval | 1 ms |
| 常规 packet | 96 bytes |
| 最大 packet | 98 bytes，允许 48/49 frame 调度 |
| product string | `Sunshine Virtual Microphone` |
| serial | 安装实例稳定，不使用会话随机值 |

Profile 不包含 HID interface、AudioStreamingOut 或扬声器 feature unit。Windows 只应创建 capture endpoint。

原型可以使用内部测试 VID/PID，但发布构建必须取得合法且不会与真实硬件冲突的 VID/PID 分配。禁止复用 Sony VID/PID，也不能把随机 VID/PID 当成长期身份。

Profile 加载时必须进行静态和运行时校验：

- 只有一个 AudioStreamingIn alternate setting。
- format 必须严格等于 mono/48 kHz/S16。
- endpoint 必须是 IN、isochronous、1 ms。
- `HMController.UsbAudio.Microphone` 存在。
- `Channels`、`SampleRateHz` 和 `BitsPerSample` 与 profile 一致。

## 7. Core/宿主协议扩展

### 7.1 消息编号

沿用现有消息区间：Core 请求使用低编号，宿主异步状态使用 `100+`。

```text
MicCreate       = 12
MicCreateReply  = 13
MicPcm          = 14
MicFlush        = 15
MicFlushReply   = 16
MicDestroy      = 17
MicDestroyReply = 18
HostStatus      = 106
MicStatus       = 107
```

`MicCreate`、`MicFlush`、`MicDestroy` 使用非零 `request_id` 并分别等待对应的 `Reply`。`MicPcm` 使用 `request_id = 0`，不逐包回复。

### 7.2 `MicCreate`

请求 payload：

```text
sample_rate_hz : u32 = 48000
channels       : u8  = 1
bits_per_sample: u8  = 16
flags          : u16
```

回复 payload：

```text
result          : i32
generation      : u32
sample_rate_hz  : u32
channels        : u8
bits_per_sample : u8
reserved        : u16
```

宿主不做隐式格式协商。请求不是固定格式时返回稳定错误码。

### 7.3 `MicPcm`

每条消息建议承载 10 ms、480 frame：

```text
generation      : u32
sequence        : u32
capture_time_us : u64
frame_count     : u16
flags           : u16
pcm_s16le       : frame_count * 2 bytes
```

Flags：

```text
STREAM_START    = 1 << 0
STREAM_END      = 1 << 1
DISCONTINUITY   = 1 << 2
SILENCE         = 1 << 3
```

约束：

- `frame_count` 首期不超过 960 frame（20 ms）。
- payload 长度必须精确匹配 header 和 frame_count。
- generation 不等于当前设备 generation 时直接丢弃并计数。
- sequence 不连续时清空旧 PCM，下一包带 discontinuity 语义。
- `capture_time_us` 只用于诊断和未来漂移控制，不作为 USB packet 的绝对调度时间。

### 7.4 `MicFlush`

`MicFlush` 清空尚未提交的远端 PCM、重置 sequence，并让 host capture 继续收到静音。它不改变 USB device、alternate setting 或 Windows endpoint identity。

### 7.5 `MicDestroy`

只在用户关闭 USB 麦克风、组件修复/升级需要释放设备或宿主退出时使用。正常 Moonlight 断开不得发送该消息。

### 7.6 `MicStatus`

状态改变时立即发送；持续活动期间最多每秒一次：

```text
generation       : u32
state            : u8
is_host_streaming: u8
reserved         : u16
buffered_bytes   : u32
underruns        : u32
dropped_frames   : u32
submit_errors    : u32
last_error       : i32
```

状态消息进入可靠控制队列，不与每个 PCM packet 一一对应。

## 8. 数据面与线程模型

### 8.1 Core 侧

网络/混音线程不得直接执行可能阻塞的 pipe write。`usbip_mic_sink_t` 持有一个有界 SPSC PCM queue 和专用发送 worker：

```text
Moonlight receive/decode/mix
  -> write_mic_pcm()
      -> enqueue 10 ms PCM
          -> virtual-device-host writer
              -> Named Pipe
```

队列策略：

- 目标积压：20 ms。
- 最大积压：60 ms。
- 队列满时丢弃最旧 PCM，并在下一包设置 `DISCONTINUITY`。
- 不允许通过等待 pipe 腾出空间来增加客户端麦克风延迟。
- 控制消息优先于 PCM；`MicDestroy`、shutdown 和 DS5 detach 不能被 PCM 饿死。

### 8.2 宿主侧

Pipe read loop 只执行帧校验和快速入队。麦克风 pump 独立于 pipe loop：

```text
Pipe ReadLoop
  -> validate MicPcm
  -> bounded PCM ring

Microphone Pump
  -> inspect IsStreaming / BufferedBytes
  -> Submit(PCM)
  -> underrun 时 Submit(silence)
  -> partial submit 时计数并丢弃未接受的旧尾部
```

生产实现使用 `ArrayPool<byte>` 或固定块池，避免每秒约 100 次持续分配。任何日志、JSON 序列化、磁盘访问或网络操作都不得出现在 pump 热路径。

### 8.3 缓冲和漂移策略

首期不引入高质量重采样器，采用有界延迟优先策略：

- `BufferedBytes` 小于约 10 ms：补静音并增加 underrun。
- 大于约 60 ms：丢弃最旧数据回到约 20 ms，并标记 discontinuity。
- `Submit()` 返回部分长度或 0：不阻塞重试旧数据，记录错误并让后续新 PCM 优先。
- 连续提交失败达到阈值：设备进入 `device_faulted`，停止高频日志并通知 Core。

后续只有在实测发现客户端时钟与 USB host clock 的长期漂移不可接受时，才加入窄范围自适应重采样。

## 9. Sunshine Core 代码结构

### 9.1 通用宿主客户端

建议新增：

```text
src/platform/windows/virtual_device_host/
  host_client.h
  host_client.cpp
  protocol.h
  process_owner.h
  process_owner.cpp
```

职责：

- 固定路径和 component manifest 校验。
- 创建随机 pipe、启动 elevated helper、加入 Job Object。
- Hello/capability 协商。
- 单一 reader 和优先级 writer。
- request/reply 关联、超时、关闭和一次性恢复。
- 宿主引用计数与设备 owner registry。

现有 `ds5_sidecar_client_t` 第一阶段保持公共接口不变，内部委托给共享的 `host_client_t`。这样不会把宿主重构扩散到输入后端调用者。

### 9.2 麦克风 sink 抽象

建议把当前 `mic_write_wasapi_t` 提升为后端之一：

```cpp
class mic_sink_t {
public:
  virtual int start() = 0;
  virtual int write_pcm(std::span<const std::int16_t> samples) = 0;
  virtual void flush() = 0;
  virtual void stop_stream() = 0;
  virtual void shutdown() = 0;
  virtual ~mic_sink_t() = default;
};
```

实现：

```text
wasapi_cable_mic_sink_t   现有 VB-CABLE 路径
usbip_mic_sink_t          新宿主/UAC 路径
```

现有 `write_mic_pcm(samples, frame_count)` 调用方无需提供时间戳。`usbip_mic_sink_t` 在将 PCM 放入发送队列时，以单调时钟生成仅用于诊断的 `capture_time_us`；未来若上游能提供源时间戳，可把它作为可选元数据传入适配层，但不得改变现有调用契约。

`stop_stream()` 与 `shutdown()` 必须分开：前者对应远端会话结束，只 flush；后者才允许销毁 endpoint。

### 9.3 配置

新增配置建议：

```text
microphone_redirect_backend = vb_cable | usbip_experimental | auto | disabled
microphone_set_default      = false
```

行为：

- `vb_cable`：保持现状。
- `usbip_experimental`：宿主或 UAC 创建失败时明确失败，不偷偷写入其他播放设备。
- `auto`：优先尝试 USB/IP；失败时仅在 VB-CABLE capture/render 对均已可用时回退，并记录最终选中的后端。
- `disabled`：不创建 sink。
- `microphone_set_default=false`：只创建 endpoint，用户在游戏中显式选择。

USB/IP 后端发布成熟前，已有用户配置的默认行为不得自动从 VB-CABLE 迁移到 USB/IP。

## 10. 宿主代码结构

建议将 `SidecarServer` 逐步拆分为：

```text
tools/sunshine-ds5-sidecar/
  VirtualDeviceHostServer.cs
  Protocol.cs
  DeviceRegistry.cs
  ControllerSession.cs
  VirtualMicrophoneSession.cs
  MicrophonePcmQueue.cs
  DefaultAudioEndpointGuard.cs
  profiles/
    sunshine-virtual-microphone.json
```

`DeviceRegistry` 负责：

- DS5 device ID 到 `ControllerSession` 的映射。
- 唯一 `VirtualMicrophoneSession`。
- 幂等 create/destroy。
- shutdown 时按“停止 pump -> 解除事件 -> Dispose USB device -> Dispose HMContext”的顺序清理。

首期无需立即重命名 namespace、项目目录或发布资产。行为验证稳定后再执行纯命名迁移，避免与数据面改动混在同一个版本。

## 11. 安全边界

1. Named Pipe 继续使用 `CurrentUserOnly`、随机名称和单实例。
2. Sidecar 在连接建立时校验 owner 进程 token/elevation；协议中的 PID、名称或 token 字符串不作为授权依据。
3. Sidecar 必须位于固定、经 canonicalize 的 active component 目录，并由 Core 校验 manifest 和 SHA-256。产品启用前还必须验证目录及文件 owner/DACL、拒绝每个路径分量和 executable 上的 reparse point，并在摘要校验至 `CreateProcessW` 完成期间持有禁止写入/替换的已验证文件句柄或采用等效原子机制；当前按路径二次打开的实现存在 TOCTOU 风险，不能仅以摘要校验视为已关闭。
4. Core 不向宿主传任意 profile 路径、DLL 路径、URL、命令行或 VID/PID。
5. 麦克风 profile 是组件内受校验资产；宿主只接受内置 profile ID。
6. `MicPcm` 做 checked arithmetic，拒绝长度溢出、非固定格式和超出 20 ms 的单包。
7. 所有 PCM 队列有硬上限；远端客户端不能借音频流让 elevated sidecar 无限分配内存。
8. USB/IP server 只绑定本机回环，不开放防火墙规则。
9. GUI 无权直接连接运行时 pipe；GUI 通过 Core 获取状态和发起测试。
10. 宿主日志不得记录 PCM 内容，也不得记录可复用 owner secret。

## 12. 故障处理与回退

| 故障 | Core/宿主行为 | 用户可见结果 |
|---|---|---|
| 组件缺失或摘要错误 | 不启动宿主 | 提示安装或修复组件 |
| 旧 Sidecar 无麦克风 capability | DS5 可继续；USB 麦克风不可用 | 提示更新组件 |
| usbip-win2 不可用 | 不循环安装；返回稳定错误码 | 实验后端失败；`auto` 可回退 VB-CABLE |
| UAC endpoint 枚举超时 | 销毁本次麦克风对象，保留宿主 | 显示设备创建失败 |
| PCM queue overflow | 丢最旧数据、标 discontinuity | 短暂缺音，不积累高延迟 |
| PCM underrun | 提交静音 | 应用持续获得静音麦克风 |
| `Submit()` 连续失败 | 麦克风进入 faulted | DS5 保持工作，提供重建操作 |
| Pipe write 停滞 | 取消 I/O、关闭 owner、终止宿主 | 一次受控恢复；避免卡住音频线程 |
| Sidecar 崩溃 | Core 每个退避窗口最多自动重启一次 | endpoint 会重新枚举并报告 generation 变化 |
| Core 崩溃 | Job Object 终止宿主 | Windows 清理虚拟设备 |
| usbip-win2 内核崩溃 | 用户态无法恢复 | 阻止默认启用和正式发布 |

自动恢复必须有 circuit breaker。建议 30 秒内第二次宿主异常后停止自动重启，避免在有疑似内核生命周期问题时反复 attach/detach。

## 13. 可观测性

Core 结构化日志至少包含：

```text
[virtual-device-host] state=ready protocol=1 capabilities=...
[virtual-mic] backend=usbip generation=3 state=idle endpoint_id=...
[virtual-mic] state=remote_active buffered_ms=20 underruns=0 dropped_frames=0
[virtual-mic] fallback=vb_cable reason=transport_unavailable
```

不在每个 PCM packet 打日志。活动期间每 10 秒输出一次聚合 debug 指标，状态改变和故障立即输出。

GUI 状态至少拆分显示：

- 运行组件：缺失、已校验、需要更新、损坏。
- USB/IP 传输：缺失、可用、版本不允许、需要重启。
- 虚拟麦克风：未创建、枚举中、空闲、主机采集中、远端活动、故障。
- 当前后端：USB/IP、VB-CABLE、禁用。
- endpoint 名称和稳定 ID。
- underrun、drop 和最近错误码的诊断详情。

## 14. 分阶段实施

### Phase 0：契约和风险基线

- 固定当前 HIDMaestro、usbip-win2、Windows build 和 HVCI 测试组合。
- 为现有 `SDS5` header、capability 和消息编解码补充 golden tests。
- 记录 usbip-win2 已知内核稳定性问题和版本选择，不在实现过程中擅自升级。
- 通过反射/运行时测试固定 `HMMicrophoneInput` 的 format、`Submit()`、`IsStreaming` 和 `BufferedBytes` 契约。

完成标准：无需创建虚拟设备即可在 CI 中验证协议 ABI；一次性 Windows 测试机能执行 microphone API smoke test。

### Phase 1：通用宿主重构，行为不变

- 从 `ds5_sidecar_client_t` 提取共享 process/pipe owner。
- 将 `SidecarServer` 内部改为 `VirtualDeviceHostServer + DeviceRegistry`。
- 保持现有 DS5 message、profile 和生命周期测试全部通过。
- 验证一个宿主可同时持有多个 DS5 session，且控制队列不会被实时反馈饿死。

完成标准：没有启用虚拟麦克风时，DS5 行为和现有发布物一致。

### Phase 2：USB 麦克风数据通路原型

- 新增 capability 和 `MicCreate/MicPcm/MicFlush/MicDestroy/MicStatus`。
- 先使用现有 composite profile 的 `UsbAudio.Microphone` 做仅限开发环境的链路验证。
- 将 mono PCM 临时复制到双声道，验证 Windows 录音机能接收到测试音和 Moonlight 语音。
- 不设置默认录音设备，不把该临时 DS5 endpoint 暴露为产品功能。

完成标准：`Sunshine PCM -> pipe -> HMMicrophoneInput.Submit -> Windows capture` 端到端通过，并有 sequence/drop/underrun 指标。

### Phase 3：独立 UAC1 capture-only profile

- 完成 `sunshine-virtual-microphone.json` 和 descriptor tests。
- 确认 Windows 只创建一个 capture endpoint，不创建 render/HID endpoint。
- 固定 serial/container identity，验证重连和 Sunshine 会话切换不产生 `(2)`、`(3)` 重复设备。
- 完成发布 VID/PID 决策前，只允许内部/实验构建。

完成标准：应用可以稳定选择 `Sunshine Virtual Microphone`，100 次 Moonlight 重连不改变 endpoint ID。

### Phase 4：Sunshine sink 和生命周期接入

- 引入 `mic_sink_t`，把 VB-CABLE 变为一个明确后端。
- 实现 `usbip_mic_sink_t` 的有界队列和 worker。
- 把当前“最后一个会话结束即释放设备”拆成 `flush/stop_stream` 与 `shutdown`。
- 当 USB 麦克风启用时，让宿主跨 Moonlight 会话存活。
- 增加明确配置和后端选择日志。

完成标准：USB/IP 和 VB-CABLE 可以独立选择；`auto` 的最终路由可诊断；现有 VB-CABLE 测试不回归。

### Phase 5：GUI、安装和诊断

- 在音频配置页加入后端选择、实验性警告、设备状态和测试按钮。
- 组件管理页把 DS5 Sidecar 描述升级为虚拟设备组件，但暂不强制改资产名。
- 只有用户明确选择 USB/IP 后端时才引导安装系统级传输。
- 测试操作创建或复用麦克风、提交短测试音并验证 capture endpoint，而不是只判断设备枚举成功。

完成标准：用户总能看到当前后端、设备状态、失败原因和恢复操作。

### Phase 6：稳定性和发布闸门

- 运行 pin open/close、音频服务重启、睡眠/唤醒、Core/Sidecar kill 和长时间音频压测。
- 在隔离测试机中使用 Driver Verifier；不在日常开发机启用针对全系统的验证。
- 覆盖 Secure Boot/HVCI、Windows build 和 usbip-win2 固定版本矩阵。
- 只有 usbip-win2 的相关 kernel pool corruption/teardown 问题有可验证的已签名修复后，才讨论默认启用。

完成标准：满足第 16 节验收条件并通过发布闸门；否则保持实验性和 VB-CABLE 回退。

## 15. 文件改动清单

### 15.1 预计新增

```text
docs/windows_virtual_device_host_implementation.md
src/platform/windows/virtual_device_host/host_client.h
src/platform/windows/virtual_device_host/host_client.cpp
src/platform/windows/virtual_device_host/protocol.h
src/platform/windows/mic_sink.h
src/platform/windows/mic_usbip.h
src/platform/windows/mic_usbip.cpp
tools/sunshine-ds5-sidecar/DeviceRegistry.cs
tools/sunshine-ds5-sidecar/VirtualDeviceHostServer.cs
tools/sunshine-ds5-sidecar/VirtualMicrophoneSession.cs
tools/sunshine-ds5-sidecar/MicrophonePcmQueue.cs
tools/sunshine-ds5-sidecar/profiles/sunshine-virtual-microphone.json
```

### 15.2 预计修改

```text
src/platform/windows/ds5/ds5_sidecar_client.h
src/platform/windows/ds5/ds5_sidecar_client.cpp
src/platform/windows/audio.cpp
src/platform/windows/mic_write.h
src/platform/windows/mic_write.cpp
src/stream.cpp
src/config.h
src/config.cpp
tools/sunshine-ds5-sidecar/Program.cs
tools/sunshine-ds5-sidecar/Protocol.cs
tools/sunshine-ds5-sidecar/SidecarServer.cs
tools/sunshine-ds5-sidecar/ProtocolSelfTest.cs
tools/sunshine-ds5-sidecar/README.md
src_assets/common/assets/web/configs/tabs/AudioVideo.vue
src_assets/common/assets/web/public/assets/locale/*.json
```

实际提交应按 Phase 拆分，禁止在一个提交中同时完成通用宿主重构、协议新增、profile descriptor、GUI 和组件资产重命名。

## 16. 测试矩阵与验收标准

### 16.1 自动化测试

| 测试 | 要求 |
|---|---|
| Protocol golden test | 所有字段小端序、长度和最大值固定 |
| capability compatibility | 新 Core + 旧 Sidecar 不发送麦克风消息；旧 Core + 新宿主 DS5 正常 |
| queue overflow | 丢最旧帧、设置 discontinuity、内存有界 |
| sequence gap | 清空旧数据，不重放陈旧 PCM |
| generation mismatch | 丢弃旧设备 PCM |
| partial `Submit()` | 不阻塞，不无限重试旧尾部 |
| owner EOF | 停止 pump 后释放所有 USB 对象 |
| DS5/mic isolation | 任一设备 destroy 不影响另一设备 |
| config migration | 旧配置保持 VB-CABLE 行为 |

### 16.2 本机功能验收

- Windows 只出现一个 `Sunshine Virtual Microphone` capture endpoint。
- endpoint format 是 48 kHz、mono、S16；没有新增 render endpoint。
- Windows 录音机、Discord、浏览器 WebRTC 和至少两个实际游戏能采集远端语音。
- 无远端 PCM 时录制结果为静音，不重复最后一段音频。
- Moonlight 断开和重连不 detach 麦克风，endpoint ID 保持不变。
- DS5 attach/detach 和 HD Haptics 工作时，麦克风持续正常。
- 麦克风创建失败时，已有 DS5 不被销毁。
- VB-CABLE 后端仍能通过现有测试音和重定向测试。

### 16.3 稳定性验收

- 连续运行 72 小时，无持续内存/handle/queue 增长。
- Moonlight 麦克风会话连接/断开 1,000 次，虚拟麦克风不重新枚举。
- Windows 应用 capture pin 打开/关闭 10,000 次，无 BSOD、pool corruption 或宿主失控。
- `audiosrv`/`audiodg` 重启 100 次后可以自动恢复或给出明确可恢复错误。
- 睡眠/唤醒、Fast Startup、用户注销/登录和 Sunshine 重启均有确定结果。
- 强制结束 Sidecar 后，自动恢复不超过一次；circuit breaker 生效。
- HVCI/Secure Boot 开启时通过同样压测。

### 16.4 性能目标

- Core PCM queue 常态约 20 ms，硬上限 60 ms。
- `write_mic_pcm()` 不等待 pipe 或 USB 请求完成。
- 音频数据不导致 DS5 input/feedback 的可观测延迟尖峰。
- 端到端新增延迟以实测记录；第一阶段目标是不因排队产生超过 60 ms 的额外延迟。

## 17. 发布闸门

USB 虚拟麦克风从“实验性”升级为默认候选前，必须同时满足：

1. 使用未修改、签名有效且版本固定的 usbip-win2。
2. 与本机 UAC1 attach/detach、pin close 和 endpoint purge 相关的已知内核崩溃问题已在该签名版本中修复并完成复现回归。
3. 第 16 节的 HVCI/Secure Boot 和长期稳定性矩阵通过。
4. 发布 VID/PID、产品字符串和设备身份策略完成法律与兼容性审查。
5. 组件 manifest、来源、SHA-256、许可证和回滚版本固定；manifest 使用明确的 `hidmaestro_file_version = 1.6.2.0` 字段，并与 `HasPinnedMicrophoneRuntime` 的 `AssemblyFileVersion` 检查一致。
6. active 组件目录的受保护 owner/DACL、全路径 reparse point 拒绝及 Sidecar 校验—启动 TOCTOU 防护通过专门安全回归。
7. 用户可以显式选择 VB-CABLE 回退，升级不会强制迁移现有配置。
8. GUI 明确区分用户级宿主组件与系统级 USB/IP 传输，安装和卸载影响在 UAC 前说明。

在上述条件未全部满足时：

- 配置项继续显示“实验性”。
- 不把现有用户自动切换到 USB/IP。
- 不删除 VB-CABLE 后端。
- 不因 PoC 音频成功就宣称生产可靠。

## 18. 实施顺序和提交边界

推荐提交序列：

1. `test: freeze virtual device host protocol contracts`
2. `refactor: share the DS5 sidecar process owner`
3. `refactor: introduce the virtual device registry`
4. `feat: add capability-gated microphone protocol messages`
5. `test: prove HIDMaestro microphone submission`
6. `feat: add the capture-only UAC1 microphone profile`
7. `feat: add the USB/IP microphone sink`
8. `feat: keep the virtual microphone across client sessions`
9. `feat: expose virtual microphone configuration and diagnostics`
10. `test: add USB audio lifecycle stress coverage`

每一步必须可以单独回滚。descriptor、内核传输版本、进程生命周期和默认后端选择不得在同一个提交中同时变化。

## 19. 实施前待确认事项

1. HIDMaestro 是否完整支持没有 HID interface 的 audio-only profile；若不支持，优先推动上游增加 audio-only device 支持，不把伪 HID endpoint 带入产品设计。
2. 发布用 VID/PID 的取得方式和稳定 serial/container identity 生成规则。
3. Sunshine 服务模式下，宿主所在 Windows session 与用户音频 endpoint 的可见性。
4. 当前固定 usbip-win2 版本在 UAC IN-only 设备上的 attach、pin close 和 teardown 稳定性。
5. `HMMicrophoneInput.Submit()` 的部分提交、buffer ownership 和线程安全契约是否需要上游书面固定。
6. 组件名称从 DS5 Sidecar 迁移为 Virtual Device Host 时的 manifest、active 目录和 GUI 升级兼容策略。

这些事项不会阻止 Phase 0/1，但第 1、2、4 项必须在独立 UAC profile 进入面向用户的实验版本前关闭。

## 20. 参考资料

- [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro)：当前用户态虚拟设备运行时及 `HMMicrophoneInput.Submit()` API 来源。
- [usbip-win2](https://github.com/vadimgrn/usbip-win2)：当前已签名 UDE bus 的上游项目。
- [usbip-win2 #181](https://github.com/vadimgrn/usbip-win2/issues/181)：与本机模拟 UAC endpoint teardown 接近的 kernel pool corruption 风险记录。
- [usbip-win2 #180](https://github.com/vadimgrn/usbip-win2/issues/180)：同版本的另一项 kernel pool corruption 风险记录。
- [Microsoft USB Audio Class system driver (Usbaudio.sys)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-audio-class-system-driver--usbaudio-sys-)：当前 UAC1 profile 使用的 Windows inbox 驱动；UAC2/`usbaudio2.sys` 仅作为未来路径考虑。
- [Microsoft UDE client driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/writing-a-ude-client-driver)：UDE bus/client driver 边界和签名责任。
