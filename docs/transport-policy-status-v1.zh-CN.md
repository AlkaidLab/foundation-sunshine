# 可靠策略状态通知协议

本文定义 Sunshine 与 Moonlight 公共库的最新策略状态通知，以及 PC/Android 的查询补偿。消息只传递可信状态进度，完整策略与请求回执仍由[配对 HTTPS API](transport-policy-api-v2.zh-CN.md)提供。可靠通知不授予控制租约，也不证明整帧到达、解码或呈现。

工作区已有实现与组件测试，完整交付继续按[实施文档 P3/P4](adaptive-fec-implementation.zh-CN.md#尚待完成的验收)验收。真实收发、应用运行与资源开销分别记录于[验证记录](adaptive-fec-validation.zh-CN.md)，不能仅凭协议编解码通过宣告闭环完成。

第 41 阶段通过原生实际收发，第 42 阶段补充完整 Qt 只读会话中的原生接收、提前唤醒实际配对查询及独立回执对账。Qt 控制会话、Android Game/JNI、完整故障与资源成本仍须验收。

## 能力协商

通知版本独立于逐包反馈 body v1、视频 profile 2、统计内层 v2 与 HTTPS API v2。

1. 主机 DESCRIBE 声明 `a=x-ss-video[0].policyStatusVersion:1`。
2. 客户端仅在已请求逐包反馈、主机支持视频 profile 2、非 controlOnly、视频和控制 v2 加密均可用时，在 ANNOUNCE 请求 `x-ss-video[0].policyStatusVersion:1`。
3. 主机核对资格，并返回 `X-SS-Policy-Status: 1`；客户端还须取得本连接的非零 feedback epoch。缺少确认或未知版本不启用通知。
4. 未请求自动控制的只测量连接也可协商通知；其控制权限保持原查询契约。旧客户端忽略新声明，新客户端连接旧主机时继续查询补偿。

停止或开始新连接时清除协商结果、接收缓存和通知序号状态。公共库不允许运行中把旧连接配置变更为新授权。

## 封装与字节布局

消息类型为 `0x550f`，使用现有加密控制 v2 与可靠 ENet。外层控制头沿用原协议字节序；下表仅定义其后固定 72 字节载荷，所有多字节整数为大端。长度必须精确匹配，不能接受截断或追加字节。

| 偏移 | 字节数 | 字段 | 有效值 |
| --- | --- | --- | --- |
| 0 | 2 | version | 1 |
| 2 | 2 | length | 72 |
| 4 | 2 | flags | 只允许下表七个标记 |
| 6 | 1 | controlSource | 0 legacy，1 manual，2 GoogCC，3 local |
| 7 | 1 | failure | 0 none，1 unsupported，2 backend_failure，3 superseded，4 stopped |
| 8 | 4 | sessionId | 非零 uint32 |
| 12 | 4 | reserved | 全零 |
| 16 | 8 | connectionEpoch | 非零 uint64，必须属于当前连接 |
| 24 | 8 | noticeSequence | 非零 uint64，仅在状态变化且成功入队时递增 |
| 32 | 8 | controlEpoch | 非零 uint64，来自当前 accepted 策略 |
| 40 | 8 | acceptedRevision | 非零 uint64 |
| 48 | 8 | encoderAppliedRevision | 已知 SDK 应用时非零，否则零 |
| 56 | 8 | firstSentRevision | 已知首发时非零，否则零 |
| 64 | 8 | firstSentFrame | 已知首发时为对应帧，帧 0 合法；未知时零 |

| flags | 含义 |
| --- | --- |
| `0x01` | 已知 encoderAppliedRevision |
| `0x02` | 已知 firstSentRevision 和 firstSentFrame |
| `0x04` | accepted 策略待应用 |
| `0x08` | 生成状态时编码器当前就绪 |
| `0x10` | 会话已停止 |
| `0x20` | 已协商逐包控制能力 |
| `0x40` | 会话快照中的视频 pacer 能力 |

已知应用版本不得大于 accepted；已知首发版本不得大于已知应用版本。不存在标记的对应字段必须为零。pending 不得与失败或该 accepted 已应用同时成立。stopped 清除 pending、当前就绪、控制及 pacer 能力标记，但不撤销历史应用或首发事实。

controlSource、failure 分别描述 accepted 策略的来源与回执；首发版本可能属于更早策略。能力标记与控制来源不能替代查询中的活动状态或实际授权。

## 状态进度与重建

接收器只接受当前 epoch 与固定 sessionId 的更高通知序号。accepted、control epoch、已知 SDK 应用和已知首发版本不得倒退；同一首发版本的帧号不可改变，同一 control epoch 的来源不可改变。拒绝不修改缓存。终止状态不能被更晚通知复活，序号耗尽后不回绕。

编码器当前就绪是可变化状态：重建开始或 SDK 失败可以从就绪变为未就绪，再在初始化成功后恢复。历史 SDK applied 和 first sent 回执仍保持。接收器不得把当前就绪当成不可倒退的进度，应用也不得因此拒绝更新的合法查询。

首次成功提交仅证明该版本首个包被 OS 接受。它不表示剩余分片全部提交，不表示接收端修复成功，也不表示参考链、播放期限或画质满足要求。

## 发送与资源界限

发送器发布最新状态，允许合并中间阶段。新状态成功入队间隔至少 250 ms；没有变化时每 2 秒发送心跳，心跳保持通知序号，但外层加密使用新的传输序号。入队失败不推进发布记录。序号到 uint64 最大值后拒绝新的状态变化，保留查询补偿，不回绕复用身份。

72 字节仅是载荷。控制封装、加密、ENet、ACK、可靠重传和 IP/UDP 头部均通过既有控制出口的共享预算计费。发布频率不能证明反向/前向总开销达标；真实重传和排队成本须按 V6 测量。

服务端 HTTPS 回执历史有界，当前为 32 项。发送器保留已经发布过的首发进度，避免其旧回执淘汰后发生倒退。协议不保证每次中间事件投递一次，也不保证断连后 STOPPED 通知能送达。

## 应用查询补偿

公共库 `LiGetTransportPolicyStatusNotice()` 返回当前连接的线程安全值副本；没有合法通知或停止后返回 false。PC 使用原 Session 的查询控制器，Android JNI 返回不可变字段副本；64 位字段保留完整无符号十进制值，不通过浮点数或有符号数值比较。

应用收到新通知时校验原 session/epoch，合并触发配对 HTTPS 查询，保留原有周期查询。查询至少追上通知中的 accepted、control epoch、历史 SDK 和首发进度；完整策略值、操作 requestId、失败详情与提交权限依赖合法查询。落后的在途回复不能更新为可提交状态，发布时再次核对最新通知。

首发回执仍在查询历史中时，版本、SDK 标记和帧号须精确对应。更早的首发回执已淘汰且查询策略已前进时，不因缺少该历史项永久阻塞；当前 accepted 的首发事实仍须有自己的回执。通知不用于猜测一个未确认的写请求是否成功，超时或冲突后不自动重新写入。

停止先撤销消费与提交资格，再排空既有查询；旧回调、旧 epoch 和旧通知均不能恢复新连接的权限。只读控制器不提交任何写入，也不把查询主机预算继承为用户的重连设置。

## 验收要求

分别验证严格线格式、畸形与未知版本、完整 uint64、重复/倒序、身份隔离、回执历史淘汰、SDK 重建、在途查询竞争、通知限频、可靠队列拒绝、停止与重连。再验证真实加密 ENet 与配对查询一致、两端实际应用消费、新旧能力组合和真实控制开销。

源码入口为 Sunshine `src/transport/transport_policy_notice.*` 和 `src/stream.cpp`、公共库 `src/TransportPolicyStatus.*`/`VideoStream.c`/`ControlStream.c`/RTSP 协商，以及两端 TransportPolicy 查询服务。三份公共库必须分别同步、构建和保存产物，不能将某一份检出通过视为三端通过。
