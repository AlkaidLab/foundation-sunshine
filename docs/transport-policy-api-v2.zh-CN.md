# 会话传输策略 API v2

本接口用于已配对客户端查询和提交自身串流会话的手动传输策略。代码入口为 `src/nvhttp/dynamic_params.cpp`，严格 JSON 契约为 `src/transport/transport_policy_json.*`，会话寻址与接受为 `src/stream.cpp`。当前实现包含会话 FEC、归一化净编码目标、策略版本、控制来源与代次以及应用回执。期限视频 owner 已接线并完整构建，隔离串流的单视频预算、显式自动接管及实验 FEC 联动已有证据；全流量预算与完整客户端业务接入仍待完成，见[实施文档](adaptive-fec-implementation.zh-CN.md)。

同一认证上下文新增实验操作 `POST /api/v2/transport-control`，用于活动会话独立切换自动码率/FEC 和更新手动总预算上限。原手动 POST 的严格字段集合保持不变；两端 UI、可靠通知和真机验收分别记录，不因服务端接口存在而宣告 P3 完成。

接口注册和源码测试不证明设备上的配置结果或网络收益。新模式必须显式提交，进入后旧码率/FEC 请求被拒绝；本阶段通过重连回到启动时的兼容模式。当前没有自动启用 GoogCC 或 RTX。

## 身份与会话

通过 GameStream 的配对 HTTPS 连接访问。身份来自本次 TLS 连接绑定的客户端证书，服务端每次请求重新检查该配对是否仍有效。请求中的名称、来源 IP 或自报 UUID 不能授予会话访问权。

成功的配对 `/launch`、`/resume` XML 回复新增可选 `transportSessionId`，是规范 uint32 十进制字符串。它仅提供寻址编号，不能授予访问权；后续查询仍校验 TLS 身份并取得连接 epoch。节点缺失或非法时，新客户端不得按 IP、名称或应用编号猜测会话；保持活动控制不可用。

`sessionId` 是启动会话编号；`connectionEpoch` 是服务端为本次连接生成的非零 64 位随机值。更新须同时匹配认证身份、会话编号、连接 epoch、当前 control epoch 和预期策略版本。会话停止后拒绝更新，迟到回执不能使状态继续推进。

所有 64 位身份、版本、计数和帧编号使用十进制字符串。只接受规范表示，不接受负号、空白、科学计数法、前导零或溢出。`sessionId` 同样用字符串传输，值须能表示为 uint32。

### 兼容控制的连接身份

升级客户端在配对 `/launch`、`/resume` 查询中发送 `transportScope=1`，与实验逐包控制开关独立。支持该扩展的服务端在成功 XML 回复中同时返回 `transportScope` 为 `1`、既有 `transportSessionId` 和新增 `transportConnectionEpoch`。epoch 在 HTTPS 启动阶段生成，同一启动票据的 RTSP 及策略状态沿用该值；每次新 launch/resume 都生成新值。随机数生成失败时拒绝启动。

客户端只在未收到 scope 确认和 epoch 两个节点时使用旧主机兼容路径；可以仍收到旧的 sessionId。确认存在却缺少身份、未知确认值、重复身份或非法值均作为握手错误，不能退为无身份写入。PC 使用完整 XML 解析；Android 的完整解析已接线并编译，JVM 只覆盖字段模型，其设备解析和 Game 握手仍待验证。

| 兼容操作 | 绑定字段 | 选择与拒绝规则 |
| --- | --- | --- |
| `GET /bitrate` | 既有码率和 clientname 查询，另加 `sessionId`、`connectionEpoch` | 绑定时按 TLS 配对身份及完整 tuple 选择；名称仅为兼容字段 |
| `GET /stream/settings` 的 bitrate/FEC 类型 | 既有 type/value/clientname 查询，另加同一对身份 | 与 `/bitrate` 使用同一归属与仲裁检查，不能成为旁路 |
| `POST /api/abr`、`POST /api/abr/feedback` | 既有 JSON 对象，另加字符串 `sessionId`、`connectionEpoch` | ABR 配置、反馈、关闭和异步任务均使用连接内状态；每个 epoch 单独清理 |

这对字段必须一起提供，使用非零规范十进制字符串；sessionId 最大为 `4294967295`，epoch 最大为 `18446744073709551615`。重复字段拒绝，ABR 正文上限为 8192 字节。`api/abr/capabilities` 的 features 新增 `connection_scope`，但具体连接是否接受扩展仍以配对启动回复为准。

```text
GET /bitrate?bitrate=4500&clientname=example&sessionId=1&connectionEpoch=42
```

```json
{"enabled": false, "sessionId": "1", "connectionEpoch": "42"}
```

| 状态码 | 含义 |
| --- | --- |
| 400 | 部分、重复、类型错误、非规范或溢出的身份；非法请求正文或码率 |
| 403 | HTTP 处理阶段不能确认有效配对身份；TLS 层仍可能先按既有认证格式拒绝 |
| 404 | 没有归属该证书的活动 tuple，包括旧 epoch；不回退为名称或 IP 选择 |
| 409 | 已要求绑定的连接收到无身份请求、无身份候选不唯一，或新控制已协商/已离开 legacy 仲裁域 |
| 200 | 兼容请求完成既有接受流程；码率成功仍只代表已提交，不能冒充 SDK 或实际发送回执 |

`/bitrate` 和 `/stream/settings` 保留 HTTP 200 外层及 XML `status_code`，客户端须读取 XML 状态；ABR 使用 HTTP 状态与 JSON 正文。校验 tuple 与提交码率/FEC 位于同一会话注册表锁域，不能检查 epoch 后再按显示名重新寻址。ABR 的 map key 包含配对身份、sessionId 和 epoch，旧关闭、旧清理或旧异步结果不得影响新连接。

未请求该扩展的旧客户端可继续使用无身份格式，但服务端先约束到本次配对证书，再要求唯一兼容候选；ABR 额外比较归一化来源地址。该路径不提供旧请求与重连的代次隔离，不能宣称与绑定连接相同的迟到写入保证。服务端内部的可信按名称控制不属于上述 HTTPS 兼容扩展；其他旧动态参数类型也不在本节证明范围内。

身份扩展不改变 legacy 码率/FEC 的历史预算定义，不授予逐包控制权，也不能绕过归一化策略仲裁。实际迟到请求、旧客户端 resume 与相应证据见[验证记录](adaptive-fec-validation.zh-CN.md)；PC/Android 完整应用兼容重连、回复丢失与完整停止成本继续单独验收。

## 查询

```text
GET /api/v2/transport-policy?sessionId=100
GET /api/v2/transport-policy?sessionId=100&connectionEpoch=42
```

首次查询可省略 connection epoch，以获得该认证身份当前会话的 epoch；之后携带它以排除旧连接。会话不存在、不属于该身份、停止或 epoch 不匹配均返回 404，避免暴露其他客户端状态。进入此接口后，未取得有效配对身份返回 403；现有 GameStream TLS 校验也会在解析 HTTP 路径前拒绝证书，以 HTTP 200 包裹 XML `status_code="401"`，或直接失败。客户端必须同时处理这类认证拒绝，不能只凭 HTTP 200 判断请求成功。

响应为 JSON，并设置 `Cache-Control: no-store`。状态包含：

| 字段 | 含义 |
| --- | --- |
| `version` | 当前为整数 2 |
| `sessionId`、`connectionEpoch`、`controlEpoch` | 本次状态的会话与版本域 |
| `controlOwner` | 当前实现为 `server`；尚不授予本地控制权 |
| `controlSource` | 运行入口区分 `legacy`、`manual`，实验显式协商和启动门槛满足后可为 `googcc`；内部状态机支持 `local`，尚未通过运行入口授予 |
| `acceptedRevision` | 最新接受的请求版本，可能还未应用或已失败 |
| `encoderAppliedRevision` | 有成功配置证据的策略版本；尚未成功初始化时为 null |
| `encoderReady` | 当前编码器是否已成功初始化且会话未停止；重建失败时为 false |
| `pending` | 最新候选尚在等待应用；失败或被替代的请求不显示为 pending |
| `accepted`、`confirmed` | 期望策略和最近成功配置的策略；尚无成功配置时 confirmed 为 null |
| `receipts` | 最多 32 项的有界历史，不保证版本连续 |
| `packetControlAvailable`、`pacerAvailable`、`wireBudgetEnforced` | 当前均为 false；不能把已计算预算当成已兑现的发送上限 |
| `experimentalPacketControlNegotiated` | 本连接已通过独立实验控制协商且尚未停止；不表示已取得控制租约 |
| `experimentalVideoPacerEnabled` | 本连接启用实验视频发送 owner 且尚未停止；不表示音频、控制等全流量预算已兑现 |
| `experimentalControllerActive` | 上述两项成立且策略来源为 `googcc`；仅反映策略归属，不是反馈新鲜度、探测能力或完整保护健康证明 |
| `experimentalAutomaticFecAvailable` | 当前为 false，自动 FEC 已移除；两端仅接受 JSON boolean true 作为可开启能力，字段缺失或类型不合法均不可用。它与活动控制能力独立，不阻断自动码率及手动预算 |
| `experimentalLiveControlAvailable` | 本版本已协商实验控制且视频 pacer 启用、会话未停止，可以提交活动会话模式请求；不证明控制器已取得租约或体验验收通过 |

策略对象的 `wireBudgetKbps` 为请求总预算，`encoderKbps` 为净编码目标；`fec.base/key/recovery` 为相对数据的保护目标。它们不等于实测码率和实际分片比例，取整、最小冗余及不可表示的大帧仍须从发包轨迹核对。

第十八阶段为 accepted、confirmed 与各 revision 回执增加只读 `encoderCeilingKbps`，无拥塞窗口回压上限时为 null。有上限时 `encoderKbps` 是预算/FEC 分配与该上限的较小值；`wireBudgetKbps` 保留网络预算，pacer 不随该独立编码上限降速。该字段不允许 POST：客户端仍须遵守原有严格请求字段集合。请求接受、SDK 应用和首帧发送继续分别确认；pending 或失败不得被展示为已经应用。手动接管会清除自动编码上限并撤销旧租约，旧 epoch 的请求不能继续覆盖新策略。

每个回执含 `encoderApplied`、`firstSentFrame` 和 `failure`。`firstSentFrame` 仅在该版本某个 UDP 包成功提交给操作系统后出现，不表示全帧发送完成、对端收到或恢复成功。冗余为零、分片可行性回退和发送失败需分别记录。

策略对象另含只读 `automaticControl`：归一化手动策略为 null；已协商启动策略或显式模式请求包含 `automaticBitrate`、`automaticFec`、`maximumTotalKbps`、十进制字符串 `activationEpoch`。启动时 activationEpoch 为 `"0"`，冻结本会话启动默认值；显式请求由仲裁器分配非零激活代次，后续自动策略和租约交接保留它。它描述不可变意图，实际应用仍检查 SDK、首发和反馈门槛；来源为 GoogCC 也可能两项自动调整都关闭。

### 原始视频包统计

当前工作区的 GET 响应提供可选 `networkStatistics`，其内层 `version` 为整数 `2`，与外层策略版本 `2` 独立。两端解析器也保留内层版本 `1` 的兼容读取。仅查询提供该字段；POST 请求字段集合和控制权规则保持原契约。统计不存在时为 null，旧服务端也可不提供。客户端将缺失、未知版本或解析失败的统计单独设为不可用，保留合法策略回执。

统计来自该配对证书、sessionId 和 connectionEpoch 对应的活动视频发送账本。它包含成功提交给操作系统的视频源包与冗余包，不包含未发送序号空洞、音频或 ENet 流量。查询过程若会话已停止或替换，可返回 null 统计；不能重新按名字选择另一会话。

| 字段 | 类型及语义 |
| --- | --- |
| `connectionEpoch`、`receiverClockEpoch` | uint64 十进制字符串；连接必须匹配外层状态，接收时钟尚未建立时为 `"0"` |
| `sampleTimeUs` | 服务端单调采样时间的十进制字符串，无法提供时为 null |
| `windowBeginUs`、`windowEndUs` | 服务端发送时间窗口的十进制字符串；窗口无效时均为 null |
| `receivedPackets`、`missingPackets`、`unknownPackets`、`sampledPackets` | 规范十进制字符串；前三项之和等于 sampled；内层版本 1 最多 4096 包，版本 2 最多 32768 包 |
| `committedPackets`、`committedIpBytes` | 当前连接成功提交的累计包数与 IP 字节数，完整 uint64 十进制字符串 |
| `missingDeclarations`、`lateCorrections`、`unresolvedEvictions` | 当前连接的累计诊断计数，完整 uint64 十进制字符串；不直接用作窗口丢包率分子或分母 |
| `fresh`、`historyTruncated` | 布尔值；分别表示生成快照时的反馈新鲜度和窗口历史是否被截断 |
| `freshnessRemainingUs` | 规范十进制字符串，范围 `"0"` 至 `"1000000"`；内层版本 2 必须提供，版本 1 可以缺失；反馈不新鲜时必须为 `"0"` |
| `reason` | `valid`、`not_negotiated`、`unavailable`、`no_samples`、`feedback_stale`、`history_truncated` 或 `coverage_incomplete` |
| `rawLossPercent` | 百分比数值或 null；仅 valid 时为 `100 × missing / (received + missing)` |
| `coveragePercent` | 百分比数值或 null；有效、非空且未截断的窗口为 `100 × (received + missing) / sampled`，可能描述过期窗口 |

当前默认窗口为 `[max(0, now−2秒), now−200毫秒]`，用实际成功提交时间选择包；最近 200 毫秒等待统计成熟，未增加播放等待。原始迟到包可以更正当前 missing 状态。unknown 保留未知语义，不能转成丢失；累计 missingDeclarations 记录历史声明，不随窗口更正而作当前丢失率计算。

fresh 要求已经协商反馈、非零接收时钟 epoch，且新的匹配变化的反馈处理时间和它覆盖的最近实际发送时间，距采样时刻均不超过 1 秒。空心跳、不匹配的报告和迟到的旧覆盖不能续期。丢包率还要求窗口有效、非空、完整覆盖和未截断；条件不足时返回 null，不能显示虚假 0%。

上述时间属于服务端时钟，客户端不能直接与本地单调时间相减。`freshnessRemainingUs` 是两个新鲜度水位剩余寿命的较小值，客户端从请求开始建立本地期限，扣除完整请求在途时间及本地持有时间，不能从收到回复起重新计满一秒。寿命缺失、耗尽、查询失败或连接停止后，业务展示撤销数值；原始策略回执不随统计过期而被改写。

当前工作区的两端已经接入本地期限、只读测量查询和原始统计控件，组件回归、Android 三种 APK、普通完整 Qt 与独立 ON/OFF 主机构建通过；归档 ON 产物的 native 实际统计 v2 查询也已验证。完整 Qt stats_only/basic 已验证统计消费、真实回复扣留/迟到时的本地过期与恢复；默认 threaded、Android 实际应用和设备验收仍单独登记，见[实施文档](adaptive-fec-implementation.zh-CN.md)。内层版本 2 扩大业务统计上限，已协商反馈的发送历史容量为 65536，legacy 保持 16384；自动 FEC 轨迹投影已移除。高于新上限时继续显示截断，不能声称完整覆盖。窗口、内存和计算成本仍须经过 V6 验证，当前常数不代表最终性能门槛。

## 提交手动策略

```text
POST /api/v2/transport-policy
Content-Type: application/json
```

下面的身份与版本只是示例，必须使用当前查询结果：

```json
{
  "version": 2,
  "sessionId": "100",
  "connectionEpoch": "42",
  "controlEpoch": "1",
  "expectedRevision": "1",
  "requestId": "manual-001",
  "budget": {
    "totalKbps": 40000,
    "otherTrafficKbps": 1000,
    "repairReserveKbps": 0,
    "probeReserveKbps": 0,
    "videoOverheadKbps": 1000
  },
  "fec": { "base": 10, "key": 40, "recovery": 20 }
}
```

所有字段必填。body 上限为 8192 字节；拒绝重复键、未知字段、过深嵌套及非整数数值。requestId 为 1 至 128 字节的字母、数字、下划线或连字符。总预算为 1 至 800000 kbps，各预留为非负整数，并受会话启动时记录的主机上限约束；FEC 目标为 0 至 100。

当前净编码目标按最高保护档保守分配：

```text
primary = total - otherTraffic - repairReserve - probeReserve
encoder = floor((primary - videoOverhead) / (1 + max(base,key,recovery)/100))
```

预留耗尽预算、净编码目标为零或超过主机上限时拒绝请求。预留会降低编码目标，不能据此宣称已发送修复或探测。独立视频 RTX 尚未实现；可选媒体探测原型使用既有视频额度内的真实包，实际共享账本将其互斥归入 probe，不能再作为新增开销扣第二次。实验会话的视频、音频和已绑定 ENet 已接实际 IP 额度，完整验收仍未完成，`wireBudgetEnforced` 保持 false。探测提交、有效估计和容量恢复分别以验证记录为准。原生探测结果目前来自私有观察者轨迹，不是本端点新增字段；更新次数不等于成功簇数。跨批发送组仍受既有额度约束，接收时钟重置不会使旧簇反馈成为新路由探测。

探测实验与自动码率同时开启时，服务端内部选择固定上游的 `PaddingDuration:0ms` 恢复配置，使未实现独立 padding 的传输不等待该流量；关闭探测或固定码率保持原配置。本端点不接受或返回该内部开关，也不返回新加的私有 padding 请求及原生 loss/RTT 诊断。原生 `NetworkEstimate` 字段不能替代原始包反馈或实测 RTT。容量恢复后的实际探测、原生接受和后续较高 SDK/pacer/线上策略已有单会话功能证据；完整容量、同版本三执行器与探测门槛及体验收益仍未验收，正式能力标志不变。

内部反馈新鲜度要求新映射变化的处理时间和覆盖包的真实发送水位均在时限内。近期处理的旧反馈保留原始统计与上游输入，但不能重新授权接管、探测或升预算；无变化报告只证明存活。接收时钟重置先清空覆盖资格。本端点未增加覆盖时间字段，`experimentalControllerActive` 也不表示当前反馈新鲜或允许探测；相关状态以私有验证轨迹核对。

实际共享额度已取得纯 IPv6 与双栈 IPv4 映射的单会话 loopback 证据。三路计费使用实际线上 IPv4/IPv6 基础头部 28/48 字节；映射地址的音频发送保留原路由，只归一化计费判断。该证据不覆盖大音频 IP 分片、隧道/扩展头、非 loopback 路径或完整公平性验收，不能据此将 `wireBudgetEnforced` 改为 true，也不新增地址族业务字段。

成功接受返回 HTTP 202，响应含 `requestId`、`requestRevision` 和当前状态。首次从 `legacy` 提交手动归一化策略时，控制来源和策略在同一锁内原子变为 `manual`，control epoch 从 1 增为 2；同一手动控制代次的后续修改仅增加 revision。调用方必须保存响应里的新 control epoch，不再沿用接管前的值。接管即时决定哪个来源可提交候选，encoder applied 与 first sent 仍由各自实际回执推进，不能因为 202 就把目标显示为已经应用。

服务端先核对当前 control epoch 和控制来源，再查最近 128 个成功请求的幂等记录。在同一控制代次内，相同 requestId 与完整内容返回原接收版本，不再创建策略；内容不同返回 409。旧代次即使命中成功历史也返回 409。因此，首次接管的旧 epoch 重试不会再次取得授权；接管响应丢失时先查询当前来源、epoch 与策略，必要时用当前 epoch 与新的 requestId 提交。GET 当前不提供按 requestId 的历史查找；相同预算不能证明原请求身份，不能推断某次历史请求是否成功。记录过期后，旧 expectedRevision 仍被拒绝。

内部 `transfer_control()` 使用连接 epoch、control epoch、来源和预期 revision 比较后交接，保留全部预算与 FEC。即使替换同一类控制器实例也增加代次；`request_controller_update()` 只接受已持有匹配租约的 GoogCC 或 local 候选。HTTPS body 不接受 `controlSource` 或自报租约，不能以额外字段取得自动控制权。手动更新可以按当前 epoch/revision 显式覆盖其他来源并产生新代次；测量请求不会执行该交接。实验 GoogCC 运行入口只在显式协商、pacing、归一化实际应用与首发、近期匹配反馈同时成立后授予租约；local 尚未授予。

实验自动启动在内部先准备归一化 revision 2，来源仍为 `legacy`、control epoch 仍为 1；随后实际门槛满足才交接至 `googcc` 并产生新代次。准备阶段不虚构应用或首发回执，归一化会话拒绝无版本的旧 ABR 更新。它不是 HTTPS 可调用的新操作；客户端查询时须同时读取策略 basis、来源和回执，不能仅根据 revision 增加推断已自动接管。

## 活动会话模式与上限

```text
POST /api/v2/transport-control
Content-Type: application/json
```

下面是字段示例，身份与版本须取自本会话当前查询：

```json
{
  "version": 2,
  "sessionId": "100",
  "connectionEpoch": "42",
  "controlEpoch": "2",
  "expectedRevision": "3",
  "requestId": "modes-001",
  "automaticBitrate": true,
  "automaticFec": false,
  "maximumTotalKbps": 40000
}
```

所有字段必填，两项模式必须是 JSON boolean；当前 `automaticFec` 必须为 false，true 返回 400 且不改变策略 revision 或控制代次。上限为 1 至 800000 的整数，并受启动记录的主机硬上限约束。沿用原接口的 8192 字节限制、身份规范、重复键/未知字段拒绝和请求号字符集。必须已协商实验控制、启用视频 pacer 并完成预算归一化；否则返回 400，不能借这个操作使旧客户端取得新模式。

请求保留当前 FEC 目标及各项预留，清除自动编码回压上限。自动码率开启时，总预算取当前值与新上限的较小值；提高上限只扩大估计范围，不立即抬高发送负荷。自动码率关闭时，明确将所提交上限作为固定总预算；净编码目标仍按 FEC 和预留重新分配。预留耗尽固定预算或净编码目标为零均拒绝。当前固定保留手动保护目标；改变 FEC 使用手动策略接口。自动 FEC 启用请求在解析阶段拒绝，不产生接受或应用回执。

每次成功请求都产生新 revision 和 control epoch，来源暂为 manual；同一模式也使用新代次，旧实例立即失去调整和探测资格。owner 取消未发送探测，使用请求的不可变策略重建控制器，保留已成功发送的账本和在途帧快照。新策略经实际 SDK 应用、该版本首包成功提交和新实例映射的近期反馈后，才授予新的 GoogCC 租约并再次递增 control epoch。探测编号映射到会话内不重复的空间，迟到旧簇不能被重建实例当作新探测；原始统计继续结算。

本操作的成功请求会立即改变代次，因此原 body 的重复提交返回 409。响应丢失时先查询当前 epoch、模式与回执，再用当前身份和新的 requestId 显式提交；不把相同目标推断为原请求已成功。错误状态沿用下表。原手动 POST 会清除 `automaticControl` 并接管，旧自动实例不可自行恢复；需要再次自动控制时显式调用本操作。两项自动开关与启动配置中的独立拥塞窗口回压开关分别控制，不将固定预算理解为禁止已单独启用的编码回压。

## 应用与失败

编码线程在输入帧提交边界处理候选。原生 NVENC/AMF 返回 SDK 配置结果；AVCodec 路径使用打开新编码器的结果，并请求新 IDR。输出帧按自身编号或 PTS 查提交策略，不能套用当前最新策略。后端部分失败时停止继续使用不确定状态，并从最近确认目标重新初始化。

| 结果 | 行为 |
| --- | --- |
| 202 | 请求已接受；读取 requestRevision 和后续回执 |
| 400 | 字段、类型、范围、预算或 JSON 无效，或请求启用当前不可用的自动 FEC |
| 403 | 无有效配对身份 |
| 404 | 会话、归属、生命周期或 connection epoch 不匹配 |
| 409 | 预期版本、control epoch 或幂等内容冲突，或查询后会话已停止 |

回执 failure 区分 `none`、`unsupported`、`backend_failure`、`superseded` 和 `stopped`。配置成功仅表示 SDK 或 codec 接受目标，实际输出可能超调；实验视频 pacing 已有实际限速证据，全流量发送上限仍未兑现。

显式实验协商后的自动码率与内部租约授予已有部分运行证据；自动 FEC 历史实现已移除，其旧结果不代表当前功能，启动默认值在会话策略中冻结；活动会话使用上述独立操作更新，手动策略端点不接受对应开关或自报租约字段。Android 与 PC 已接配对查询、严格身份/状态模型、独立活动开关、总预算与手动接管及分层回执展示，查询与写入由独立线程执行，冲突或回复丢失不自动重写。独立版本 1 的[可靠 ENet 状态通知](transport-policy-status-v1.zh-CN.md)已实现服务端、公共库与两端消费，并通过原生实际通知和配对查询对账；通知不携带或授予租约。完整两端应用的通知触发查询、可靠通道故障与通知成本继续按 P3d 验收，兼容回退也按其实际运行范围登记。

Android 控件设备测试已构建，但手机 USB 安装被系统限制，尚未执行。PC 的完整构建及 offscreen 菜单/后台组件测试通过；软件界面后端的实际 Session 完成 13 次生产菜单操作，后续 D3D11 配合 `QSG_RENDER_LOOP=basic` 也重复通过 13 步、83 次独立配对查询及 26 个策略版本，并有实际解码/渲染。默认 threaded 渲染循环、桌面合成、完整故障矩阵和性能仍须验收，详见[验证记录](adaptive-fec-validation.zh-CN.md)。组件和离线样本不代替应用串流验收。

连接生命周期以 connection epoch 区分，客户端保留最近确认的总预算用于新握手时，不得继承旧控制代次、pending、回执或自动授权。重连先核对主机当前运行的应用，再选择 launch/resume；每次连接尝试使用新串流密钥及独立回调配置。新连接收到真实 decode unit 后才能判为视频已开始，push/pull 路径分别覆盖。以上是客户端实施约束；具体实现与通过范围见实施文档的连接生命周期验收及验证记录，不增加 API 字段或修改 HTTP 状态含义。Android 已保存最后通过校验的 accepted 总预算用于新握手，停止时清空旧 view；同 revision 的完整已知 v2 策略须一致，最近 64 项历史中的 SDK/首发回执不能倒退。缺少可空状态字段与显式 null 分别处理，回复丢失不推断原请求身份。44 份真实 PC 重连状态已在 Android 组件回放通过，Game 设备握手与恢复仍未验收；兼容模式的当前连接确认仅替代旧 API 目标，不代表总线上预算或 SDK 回执。

FEC 字段是相对数据分片的名义目标，实际冗余受取整、最小冗余与块限制影响；接受目标不等于恢复概率或播放期限已经达标。内部实验选择器区分严格经验回放目标与尽力保护，尚未作为本 API 的当前质量状态返回；不能从较高保护比例推断目标已满足。归一化会话不能把控制来源改回无版本的旧 ABR，以免迟到旧请求重新生效；当前返回兼容模式仍需重连。Android 已实施连接内旧请求串行化和停止排空，新 launch/resume 等待原连接的客户端工作终态；PC 已校验旧 XML 回复并在明确确认时清除旧预算记忆。旧端点仍无 connection epoch，客户端 HTTP 超时不代表主机处理已取消，不据此承诺跨重连的迟到写入隔离；有身份的兼容操作与实际应用故障仍待交付。这些客户端实现未改变现有 v2 字段或 HTTP 状态含义。完成 SDK 失败、重建成本及完整客户端消费验证之前，不将本接口视为完整自动联动已验收。
