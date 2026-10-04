# 视频逐包反馈协议：协商 profile 2，消息体 v1

本契约对应三份 common c 共用的 `src/TransportFeedbackWire.*`、`src/VideoPacketFeedback.*` 和 Sunshine 的 `src/transport/transport_feedback_wire.*`。已接入编解码、双方协商、完整包身份、可靠水位、有界报告生成与加密 ENet 发送入口。至 2026 年 10 月 4 日，已有组件、ARM64 核心测试及真实 PC 串流的提交、反馈和策略回执对账；Android 应用入口与构建已接入，实际应用及设备验收仍待完成。测量协商不授予自动控制权；固定 GoogCC 通过另行协商的控制能力和策略仲裁取得权限，突发回放驱动的自动 FEC 已移除。完整闭环边界见[验证记录](adaptive-fec-validation.zh-CN.md)。

## 协商与边界

协商 profile 与控制消息体版本是两个独立域：

| 版本域 | 当前值 | 定义与用途 |
| --- | --- | --- |
| RTSP packet feedback profile | 2 | `TF_PACKET_FEEDBACK_PROFILE_VERSION`；完整身份、包长调整及 parity 保留 RS 符号的契约 |
| READY / REPORT 消息体格式 | 1 | `TF_WIRE_VERSION`；下面两类控制消息的 version 字段及原有字节布局 |

English version note: RTSP packet feedback profile 2 negotiates the video identity and RS parity contract. READY and REPORT retain body format version 1. These version domains are independent; a profile change does not change the body codec.

Sunshine 在 DESCRIBE 中广告 `x-ss-video[0].packetFeedbackVersion:2`。客户端默认关闭此功能；显式调用 `LiSetVideoPacketFeedbackEnabled(true)` 且收到匹配 profile 2 的广告后，在 ANNOUNCE 中请求同名属性值 2。只在本次视频会话支持并启用 `SS_ENC_VIDEO`、控制协议为 13 且 `SS_ENC_CONTROL_V2` 启用时接受。客户端在明确请求后会为测量启用视频加密，LAN 也须测量其计算成本；control only 会话不请求此能力。

接受后，ANNOUNCE 响应同时返回 `X-SS-Packet-Feedback: 2` 与 `X-SS-Transport-Epoch: <canonical uint64 decimal>`。epoch 非零，无符号、无前导零、无空白。客户端验证确认值、协商条件及完整 epoch；无确认则沿旧视频格式启动，不依据名称、IP 或仅有广告猜测已接受。之后 READY 与 REPORT 必须经过本次加密 ENet 控制通道。

旧内部 profile 1 构建要求 parity 的 streamPacketIndex 与传输序号低 24 位匹配；该假设会破坏 RS 符号或拒绝正确的 parity，不能与 profile 2 混用。新客户端收到旧 host 的 profile 1 广告时，在 ANNOUNCE 之前不请求此扩展，不扣减身份的 16 字节，也不因反馈设置额外强制视频加密；旧客户端收到 profile 2 广告时同样走其不支持版本的旧格式分支。新 host 只接受属性值 2，普通旧格式视频仍可使用原有加密能力。设置反馈请求只表示在双方支持时启用，不能强迫不支持的 host 使用新 profile；意外的确认头或错误版本须拒绝握手。核心协议新旧四组合已有真实串流与离线解码证据，完整 Qt 和 Android 应用及开关重连仍须各自验收；历史第六阶段证据保持其原始 profile 1 版本域。

English compatibility note: Profile 1 and profile 2 are not interchangeable. A version mismatch disables the extension before ANNOUNCE and retains the legacy video layout. Unexpected confirmation headers fail the handshake; enabling the feedback setting does not require unsupported hosts to negotiate profile 2.

新编号为服务端到客户端的 `0x550d`（READY）和客户端到服务端的 `0x550e`（REPORT），不覆盖两方向已有的 `0x5502`。外层继续使用既有控制封装的字节序及 AES-GCM nonce 分配；下面的消息体全部采用网络字节序，禁止直接发送 C 结构体。

服务端目前每 250 ms 可靠发送 READY。客户端可在首个 READY 前记录已认证视频，但只在收到有效 READY 后生成报告。connection epoch 从 ANNOUNCE 确认冻结，重连重新协商。双方新反馈会话的加密控制序号耗尽前拒绝继续发送；心跳发送失败随后停止连接。服务端视频 nonce 计数耗尽前也停止，不允许回绕复用。

## 完整视频包身份

仅在双方明确接受的新模式中，视频 AES GCM 明文为 `connectionEpoch uint64 | extendedSequence uint64 | existing RTP datagram`。前 16 字节使用网络字节序，随 RTP 一起认证和加密；放在 Reed Solomon 符号之外，解密验证后先剥离身份，再将原 RTP 数据报交给现有恢复队列。每个数据与冗余包都携带身份，epoch 非零，序号 `UINT64_MAX` 保留不用。

客户端对所有包核对完整序号的低 16 位与 RTP sequenceNumber，对数据分片另核对低 24 位与 streamPacketIndex，并验证原视频头及 FEC 范围。奇偶分片的 streamPacketIndex 是 RS 符号，必须保留编码结果，不能在 RS 后改写为传输序号；解析器对此返回 UINT32_MAX 哨兵，仅跳过该字段的一致性检查，完整认证身份与 RTP 检查仍执行。身份不符的包拒绝进入统计和恢复队列；重连旧包与跨多次回绕的迟到包不能被认成新包。RS 生成的数据包不产生原始接收记录。

SDP 的现有 RTP packetSize 额外扣除 16 字节，以保持既有外层数据报上限；视频加密头的原 32 字节扣减继续保留。服务端出口按实际加密头、完整身份、RTP、UDP 和 IP 字节入账。新增身份会减少每包视频数据，且当前实现增加密文缓冲与拷贝；这两项必须进入 V6 成本测量，构建或组件测试不能证明性能非劣化。

## READY

消息体固定 40 字节：

| 字节偏移 | 字段与宽度 | 语义 |
| --- | --- | --- |
| 0、2 | version、length，各 uint16 | 1 和 40 |
| 4、6 | flow、reserved，各 uint16 | 视频 flow 为 1；reserved 为零 |
| 8 | connectionEpoch，uint64 | 非零，本次服务端连接域 |
| 16 | submittedThroughExclusive，uint64 | 最大成功提交序号加一；没有成功提交时为零 |
| 24 | senderSampleTimeUs，uint64 | 服务端单调时间，须能表示为 int64 |
| 32、34 | reportIntervalMs、maxPacketsPerReport，各 uint16 | 服务端当前给出 50 ms、256 包 |
| 36 | maxFeedbackWireBytesPerSecond，uint32 | 服务端当前给出 125000 字节/秒 |

水位只由成功 OS 提交推进。水位以下可能有发送失败造成的序号洞，必须与成功提交账本求交，不能将整个区间当成已发送集合。它也不证明网卡出线或对端到达。

## REPORT

固定头为 52 字节，后接状态位图和 received 包的到达年龄：

| 字节偏移 | 字段与宽度 | 语义 |
| --- | --- | --- |
| 0、2 | version、length，各 uint16 | 1 和整个消息体精确长度 |
| 4、6 | flow、reserved，各 uint16 | 1 和零 |
| 8、16、24 | connectionEpoch、reportSequence、receiverClockEpoch，各 uint64 | 三者均非零；报告序号在连接内递增，接收时钟域改变须重建时间映射 |
| 32 | receiverSampleTimeUs，uint64 | 接收端报告生成时的单调微秒，须能表示为 int64 |
| 40 | baseExtendedSequence，uint64 | 连续报告范围的起点 |
| 48、50 | packetCount、reserved，各 uint16 | 1 至 256，且末尾序号不得溢出；reserved 为零 |

状态区长为 `ceil(packetCount/4)` 字节。每包占 2 位，同字节按从低到高排列，取值 0=pending、1=received、2=missing、3=unknown；最后一个字节的未用位必须为零。

随后按包序号顺序，仅为 received 包追加一个 uint24：`receiverSampleTimeUs - firstArrivalTimeUs`，单位为微秒。允许不同序号的首次到达时间乱序，不能改写已报告的首次到达。年龄最大为 16777215 微秒；超出表示范围时拆分、换采样基准或声明未知，不能截断。其他状态无时间字段；本地 C API 用 `UINT64_MAX` 表示无到达时间。

消息体最大为 884 字节。精确长度、版本、flow、保留位、计数、时间和序号范围任一无效时，整个消息被拒绝，不能留下部分入账。畸形报告不消耗账本的 reportSequence；有效但重复或过旧的报告不重复结算。

## 入账、负荷与序号对齐

Sunshine 使用同一会话互斥锁串行处理 UDP 成功提交和加密控制反馈。只针对已登记的包产生收到、首次缺失和一次迟到更正；未提交、历史过期、未知与 pending 不制造网络丢包。已入账的成功提交与有效反馈由发送 owner 交给固定版本 GoogCC；自动码率另检查协商、策略租约、编码器及首发回执与反馈新鲜度。手动 FEC 保持固定，原始丢包统计不再驱动自动保护选择。

服务端按消息体加 96 字节的正常 UDP/IP、ENet 与加密封装余量计入反馈 token bucket。当前速率上限为 125000 字节/秒，突发上限为 7840 字节；超额在解析前拒绝。它是测量阶段的候选资源界限，不是 V6 已冻结的性能结论。客户端发送器须兑现同一界限，并为输入、心跳和可靠控制保留调度机会；实际反向流量和尾部延迟仍待测量。

数据分片的 24 位 streamPacketIndex 和所有包的 16 位 RTP 序号只作一致性检查，奇偶分片的 RS 字段不作传输身份。`TfUnwrapSequence24()` 保留为诊断辅助，不能作为报告的权威身份：水位也无法排除跨多个回绕的旧包。报告使用视频密文内的完整身份；本地 sequence epoch 仅表示观测历史失效，不代替服务端连接身份。

客户端保留 8192 个候选槽，经过当前 30 ms 宽限生成 missing，READY 提供未见帧与尾包的候选覆盖。压力溢出声明 unknown，最终仍由服务端成功提交集合确定丢包。READY 超过 1 秒未更新时停止新报告；重放、退回水位、时钟倒退、连接身份错误和本次参数变化均拒绝。

报告使用 UNSEQUENCED ENet 包；每个周期最多生成 8 个，每次状态变化优先发送，并保留第二份尽力重复。只有成功加入 ENet 发送队列才清除对应变化，排队成功不等于远端已收到；排队失败仍消费已预留序号和预算。准备报告后发生迟到的更正不会被旧排队回执清除。两份反馈均丢失时服务端保持无覆盖，不将其当作视频缺失。

## 下一步验收

PC 已有 `MOONLIGHT_VIDEO_PACKET_FEEDBACK=1` 的显式实验入口，请求 `MOONLIGHT_VIDEO_PACKET_CONTROL=1` 时也会请求反馈。Android 已将实验反馈设置、连接业务、Java/JNI 和网络统计入口接入应用；请求控制时也会请求反馈，control only 会话除外。两端默认值仍关闭。已有真实 PC 故障与回执对账，Linux 真实部分 OS 提交另取得组件证据；继续补完整 owner、反向拥堵、生命周期及 Android 实机验收。协议和客户端核心测试，包括 ARM64 真机执行，均不替代实际应用与设备验收，见[验证记录](adaptive-fec-validation.zh-CN.md)。

当前 profile 2 只定义原有媒体数据与 RS 冗余的认证身份，没有独立 padding 包契约。媒体探测使用真实媒体，数量不足时可以等待或取消；2026 年 10 月 4 日固定 FEC 的容量恢复实验因此未通过完整探测与上限恢复门槛。补独立探测须另行明确协商、包识别与丢弃、序号及 nonce 分配、实际预算入账，并保证其提交不冒充编码帧首发回执；不能将其当作当前协议已经支持的能力。
