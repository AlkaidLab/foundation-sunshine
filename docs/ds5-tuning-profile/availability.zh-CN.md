# DualSense 调校键：哪些不可用、为什么、怎么变成可用

状态：2026-11-19。依据是 `src/ds5/tuning_runtime.h` 的 `key_implemented()`；`tests/unit/test_ds5_tuning.cpp` 的「ReportsEveryKeyWithAHostConsumerAsImplemented」逐个键核对这张表。

106 个键里，Windows 发布构建上**96 个主机已经在用**（含 35～73 的摇杆、扳机、体感整形，74～80 触摸板，24～34 中除 31 以外的自适应扳机，以及本次的 101），**10 个不可用**。非 Windows 构建没有原生 DualSense 通道，不可用的键更多，原因代码是 `platform`。

## 不可用的 10 个键

| 键 | 名称 | 原因（界面上写的话） | 能不能变成可用 | 要改哪里 | 工作量 | 风险 | 将来落在 Platform 的哪个 Module |
|---|---|---|---|---|---|---|---|
| 10 | `ds5.haptics.bt.fallback` | 蓝牙由客户端决定，主机只走 USB | 在客户端设置里已经能调（Moonlight Enhanced）。要从主机页面改，需要调参通道 | 主机、客户端、传输 | 随通道（大） | 新线格式，旧客户端收不到 | `interaction.haptics.audio`（`ir-haptics`、`authored-haptics-audio`）加 `interaction.gamepad.feedback` |
| 98 | `ds5.audio.btFallback` | 同上 | 同上 | 同上 | 随通道 | 同上 | `interaction.haptics.audio` |
| 100 | `ds5.compat.btOutput` | 同上 | 同上 | 同上 | 随通道 | 同上 | `interaction.gamepad.feedback`（`controller-feedback`） |
| 105 | `ds5.feedback.target` | 反馈去向由客户端决定 | 同上 | 同上 | 随通道 | 同上 | `interaction.gamepad.feedback` |
| 102 | `ds5.sync.allowHostOverrides` | 需要 ALK-DS5T/1 调参通道 | 通道本身 | 主机、`moonlight-common-c`、客户端 | 大 | 控制流新增消息；没协商的对端根本不发 | DS5 Module 的 profile schema，加 `session.control` 消息 17 |
| 103 | `ds5.sync.allowClientOverrides` | 同上 | 同上 | 同上 | 大 | 同上 | 同上 |
| 104 | `ds5.sync.maxRateHz` | 同上 | 同上 | 同上 | 大 | 同上 | 同上 |
| 96 | `ds5.audio.micRoute` | 主机上的 DualSense 麦克风没有采集源 | 可以做：主机已有客户端麦克风重定向（`mic_mixer`、`uac1_microphone`），把它接到 DS5 的等时输入端点，现在那个端点回的是静音 | 主机 | 中 | USB 音频时序；VM 上要实测 | `interaction.gamepad.virtual-device`（`composite-usb-virtual-gamepad`）加音频采集 |
| 97 | `ds5.audio.preamp` | 输出数据传不了扬声器前级设置 | 现有 0x5515 扩展没有这个字段；要做得加字段并要客户端新版本 | 主机、客户端、线格式 | 中 | 改我们自己的扩展格式 | `interaction.gamepad.feedback`（反馈 ABI 的音频字段） |
| 31 | `ds5.trigger.adaptive.slopeScale` | 手柄没有斜坡扳机类型 | 做不到：斜坡只能当成十段 0x21 块发出，主机分不清它和多点反馈，没有可以放大缩小的块 | — | — | — | `interaction.gamepad.feedback`（只有扳机效果模型引入斜坡语义才可能） |

## 本次变成可用的

| 键 | 名称 | 做法 |
|---|---|---|
| 101 | `ds5.compat.reportRateCapHz` | 主机向客户端请求体感上报的频率取这个值（默认 250 Hz，30～1000），保存后正在串流的手柄立即收到新请求。客户端按「主机请求」和「自己的上限」取小，再受手柄采样率限制。不需要调参通道：请求走原来的「设置体感上报」控制消息。 |
| 将来落在 | `interaction.gamepad.capture`（客户端采样节拍）和 `interaction.gamepad.wire`（请求报文） | |

## 原简报里有、实际已经可用的

2（触觉模式）、24～30、32～34（自适应扳机按模式缩放）、35～73（摇杆、扳机轴曲线、体感滤波和方向）、74～80（触摸板：主机指针或精确触控板）、106（身份）在 Windows 发布构建上都有主机侧的执行者，不在不可用表里。
