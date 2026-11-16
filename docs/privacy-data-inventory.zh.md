# 隐私数据清单：每个客户端发送什么、服务器保存什么

这份清单回答三个问题：发送了什么、为什么需要、怎样去除了个人信息。每一条都标出了代码位置（`文件:行号`）；代码位置由 `scripts/telemetry/build_inventory.py` 在生成时查找，所以会随代码更新。

## 一句话

两个独立的开关（错误报告、使用统计），默认开启，关闭后该类内容不再发送。数据只发往开发者自己的服务器 `api.alkaidlab.com`（Cloudflare Worker），不经过广告网络，不出售。除用户亲手写的反馈、随崩溃或反馈报告附带的匿名程序日志、严重串流问题之后的日志摘录和错误报告携带的组件日志最后几行（仅在错误报告开启时，清洗后的文本，见「从不发送」）外，没有任何自由文本；名称、路径、地址、账号在发送前就不在载荷里。

服务器自己不保存 IP 地址，只保存 Cloudflare 给出的国家代码；Workers 日志已关闭 `worker:wrangler.toml:12`。连接本身必然经过网络，Cloudflare 作为网络服务商在传输层看得到连接的 IP，这一层不由我们控制，我们的代码不读取、不保存、不记录它。

## 每个批次都带的信封

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| install_id | `3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11` | `src/crash/crash_core.cpp:609` `src/crash/crash_core.cpp:593` | 统计日/周/月活跃安装数；把同一台安装的错误归在一起。 | 否：安装时随机生成的 UUID v4，与硬件、名称、账号无关。 | 服务器只存 install_hash（SHA-256 的前 32 位十六进制）`worker:src/crypto.ts:19`，不存原值。 | activity、sessions 400 天；events 180 天。 |
| session_id | `9a1c6b0e-52d4-4f1a-8e3b-7c2d5e9f0a13` | `src/telemetry/events.cpp:736` | 区分程序的每一次运行（计算无崩溃会话比例）。 | 否：每次启动重新随机生成。 | 随会话摘要保存。 | sessions 400 天。 |
| product, channel, os, arch | `host`, `stable`, `windows`, `x64` | `src/telemetry/events.cpp:736` `src/telemetry/telemetry.cpp:96` | 按产品、发布通道、系统和 CPU 架构拆分所有数字。 | 否：固定词。 | 发送端和 Worker 都用固定词表 `worker:src/schema.ts:165`。 | 随所在记录。 |
| version | `2026.1112.0.杂鱼` | `src/telemetry/events.cpp:712` | 知道哪个版本出了问题。 | 否：程序自己的版本号。 | 只允许字母、数字和 `. _ + -` 空格，其余置空 `src/telemetry/events.cpp:712`；Worker 用同一规则 `worker:src/schema.ts:165`。 | 随所在记录。 |
| os_version | `10.0.26100` | `src/telemetry/telemetry.cpp:211` `src/telemetry/events.cpp:721` | 发现只在某个 Windows 版本上出现的问题。 | 否：系统版本号，数百万人相同。 | 只允许字母、数字和 `. _ + -` 空格，最长 48 字符 `src/telemetry/events.cpp:721` `worker:src/schema.ts:165`；不符合的不再「清洗后发送」，而是直接丢弃。 | sessions 400 天。 |
| gpu_vendor | `nvidia` | `src/telemetry/telemetry.cpp:125` | 发现只在某个显卡厂商上出现的问题。 | 否：四个词之一（nvidia、amd、intel、apple）或 `other`；不含显卡型号和驱动字符串。 | 厂商先被归为一个词 `src/telemetry/telemetry.cpp:125`；Worker 只接受短的小写标记。 | sessions 400 天。 |
| locale | `zh-CN` | `src/telemetry/events.cpp:728` | 知道哪些语言需要优先照顾。 | 否：语言标签。 | 必须符合语言标签格式，否则置空 `src/telemetry/events.cpp:728` `worker:src/schema.ts:165`。 | sessions 400 天。 |
| country（服务器添加） | `CN` | `worker:src/http.ts:67` | 了解用户所在地区，用于语言和网络方面的决策。 | 弱：只到国家，不到城市和地址。 | 只保留 Cloudflare 给出的国家代码；Worker 不读取、不保存、不记录 IP 地址；Workers 日志已关闭 `worker:wrangler.toml:12`。 | sessions、activity、crash_reports 400 天。 |

## 使用事件（使用统计）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| app_start：cold、start_ms、update_source、update_source_miss | `true`, `1800`, `cnb`, `version_lag` | `src/telemetry/events.cpp:326` | 启动耗时；最近一次更新实际来自哪里（Mirror酱、CNB 或 GitHub），选了 Mirror酱 却没有用上时的原因（还没同步该版本、CDK 被拒、额度、网络、没有 CDK）。 | 否：一个布尔值、一个数字、四个词之一、五个词之一。 | 按类型和范围的白名单 `src/telemetry/events.cpp:326`。 | events 180 天。 |
| app_exit：uptime_s | `3600` | `src/telemetry/events.cpp:327` | 程序运行多久；会话是否正常结束。 | 否：一个数字。 | 范围 0 到一年。 | events 180 天。 |
| opt_out：choice | `usage` | `src/telemetry/events.cpp:438` | 用户关闭了两个开关之一（匿名使用统计或问题自动上报）；每次关闭时每个产品发一条（关闭后再打开、再关闭会再发一条），不算作使用。 | 否：两个固定词之一。 | 这一批只含安装编号、新的会话编号、产品和版本；由主机自己发送；最多重试 14 天；该开关再次打开之前不再发送它的任何内容。安装编号在待发送记录里保留，直到发出或过期。 | opt_outs：400 天。 |
| stream_start / stream_end：client_kind | `moonlight-macos-enhanced` | `src/telemetry/events.cpp:328` `src/telemetry/stream_stats.cpp:92` | 哪些客户端家族在使用，用来决定投入方向。 | 否：只能是主机自己识别的六个家族之一，否则为 `other`。用户给设备起的名字不会传出（修复前这是一个自由的 catalogue id）。 | 发送端白名单 `src/telemetry/stream_stats.cpp:92`，Worker 端枚举 `worker:src/schema.ts:53`。 | events 180 天。 |
| stream_start / stream_end：client_os、codec、width、height、fps、hdr、audio_codec、transport | `macos`, `hevc`, `2560`, `1440`, `120`, `true`, `opus`, `altp` | `src/telemetry/events.cpp:328` `src/telemetry/stream_stats.cpp:122` | 人们串流用什么格式和分辨率；ALTP 传输用得多不多。 | 否：固定词和数字。 | 发送端 `src/telemetry/events.cpp:328` 和 Worker `worker:src/schema.ts:53` 都用枚举；数字被限幅。 | events 180 天。 |
| stream_start / stream_end：app_kind | `sparxie` | `src/telemetry/stream_stats.cpp:56` | 桌面、大屏、Sparxie、Steam 或其他：哪类使用最重要。 | 否：五个词之一。应用名称和命令只用来选词，不会发送。 | `src/telemetry/stream_stats.cpp:56` | events 180 天。 |
| stream_start / stream_end：sparxie_game | `genshin` | `src/telemetry/stream_stats.cpp:102` | 哪些 Sparxie 游戏在使用（只用目录里的 id）。 | 否：Sparxie 目录里的九个 id 之一，或 `other`；手写或未知的值变成 `other`。 | `src/telemetry/stream_stats.cpp:102`，Worker 端枚举 `worker:src/schema.ts:53`。 | events 180 天。 |
| stream_end：duration_s、result | `600`, `normal` | `src/telemetry/events.cpp:332` | 串流总时长（「串流分钟数」的来源）；串流以什么方式结束。 | 否：一个数字和四个词之一。 | `src/telemetry/events.cpp:332` | events 180 天；sessions 保存合计 400 天。 |
| feature_used：feature | `stream.hdr`, `web.app_added`, `panel.pair_open` | `src/telemetry/events.cpp:333` `src/telemetry/service.cpp:228` | 哪些功能真的有人用。 | 否：我们自己的 id。网页只能用自己产品前缀（`web.` 或 `panel.`）的 id `src/telemetry/service.cpp:228`。 | 小写标记 `src/telemetry/events.cpp:333`；网页事件的前缀检查 `src/telemetry/service.cpp:228`。 | events 180 天。 |
| setting_changed：key、value | `max_bitrate`, `15000-30000` | `src/telemetry/events.cpp:334` `src/telemetry/events.cpp:584` | 人们改了哪些设置、改成什么类型的值。 | 否：key 来自 27 项名单；value 是名单里的词或区间。路径、名称、地址这类自由值不在名单里。 | `src/telemetry/events.cpp:584`，Worker 重复检查名单和取值格式 `worker:src/schema.ts:61`。 | events 180 天。 |

## 体验事件（使用统计）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| stream_quality（每次串流一条） | `fps_p50 119`, `encode_ms_p95 6`, `loss_pct 0.1`, `reconnects 0` | `src/telemetry/events.cpp:357` | 串流是否流畅？按版本和编码看帧率、延迟、丢包、卡顿。 | 否：数字。 | 限幅并四舍五入到一位小数 `src/telemetry/events.cpp:357`。 | events 180 天；AE 数据点按 Cloudflare 的保留期。 |
| stream_health（每串流小时一条） | `windows 60`, `bad {backlog_p95: 3}`, `queue_flushes 0`, `codec hevc` | `src/telemetry/events.cpp:407` | 新版本的串流是否比上一版更差？按分钟统计主机处理、编码、采集、发送耗时和发送积压，有多少分钟越过固定界限，队列被清空、关键帧请求、丢包；一部分串流还带每个指标的 19 桶分布。 | 否：数字和固定词（编码、分辨率、帧率、编码器、采集方式、显卡驱动版本）。 | 只收白名单里的名字，数字限幅，维度是固定词 `src/telemetry/events.cpp:407`。 | 计数 400 天；不保留原始行。 |
| stream_anomaly | `code XP_QUEUE_FLUSH`, `value 31`, `count 992`, `log true` | `src/telemetry/events.cpp:424` | 发生了哪种问题、多严重、在什么配置上；严重时还会发送问题前两分钟的主机日志（已匿名）。 | 低：固定代码和数字；日志片段是清洗后的文本，没有时钟时间，清洗后日志行里剩下的内容会被发送。 | 只收固定代码；日志片段丢弃引号、方括号和花括号里的文字（只装代码的短方括号如 `[E12]` 保留），用和崩溃日志相同的清洗规则，并去掉计算机名和账户名 `src/telemetry/events.cpp:424`。 | 计数 400 天；日志片段 90 天。 |
| stream_regress | `metric jitter_p95`, `baseline 0.3`, `current 2.6` | `src/telemetry/events.cpp:440` | 这台安装是否比它自己以前更差？一个指标的名字、以前的值和现在的值；每个版本最多一次。 | 否：指标名和两个数字；滚动平均保存在本机自己的目录里。 | 指标名固定，数字限幅 `src/telemetry/events.cpp:440`。 | 计数 400 天。 |
| startup：host_start_ms、first_frame_ms | `2100`, `450` | `src/telemetry/events.cpp:442` | 主机多久就绪、多久出第一帧。 | 否：数字。 | 限幅。 | events 180 天。 |
| panel_page：page、time_to_content_ms | `apps`, `180` | `src/telemetry/events.cpp:443` | 控制面板各页面多久显示出内容。 | 否：我们自己的路由 id 和一个数字。 | 标记和范围。 | events 180 天。 |
| page_vitals：page、visits、lcp、inp、cls、loaf、fpaint、tti、heap、loaf_n、loaf_block_ms、fr_a..fr_d、dwell_s | `home`, `15`, `lcp [0,15,0,0,0,0,0,0]`, `fr_d 5` | `src/telemetry/events.cpp:565` `m3e:src/telemetry/pagePerf.js:77` | 面板和 Web 界面用起来是否流畅？按页面和每次发送：最大内容绘制、点击响应、版面跳动、最长的忙碌帧、首次绘制、页面自报可用的时间、页面内存各落在八个固定区间的哪一个，以及滚动或动画时超过 17、34、50 毫秒的帧数。 | 否：每个区间的次数、我们自己的页面名和几个计数。没有单次事件的轨迹和时间戳，没有文字。 | 每个指标八个计数，区间边界是约定的一部分 `m3e:src/telemetry/pageBuckets.js:8`；页面名按固定格式检查，计数限幅 `src/telemetry/events.cpp:565`；服务器按版本和日期把计数相加 `worker:src/pagevitals.ts:81`。 | 计数 400 天；按页面的部分在 Analytics Engine。 |
| api_latency：endpoint、calls、errors、timeouts、lat | `config`, `40`, `0`, `0`, `lat [0,40,0,0,0,0,0,0]` | `src/telemetry/events.cpp:584` `m3e:src/telemetry/pagePerf.js:77` | 主机回答面板和 Web 界面的请求有多快？按接口名：请求数、失败数、超时数和八个耗时区间。 | 否：接口名（路径去掉数字和 id，至多两段）和计数。 | 只有数字的段和过长的段不进名字，上报本身不计时，页面自己取消的请求不计 `m3e:src/telemetry/pagePerf.js:77` `src/telemetry/events.cpp:584`。 | 计数 400 天。 |
| device_facts：gpu_vendor、gpu_driver、os_build、displays、refresh_hz、nic_link、nic_speed | `nvidia`, `32.0.15.6094`, `26100`, `2`, `144`, `wired`, `2_5g` | `src/telemetry/events.cpp:649` `src/telemetry/telemetry.cpp:219` | 哪类电脑出了问题？粗略的配置，只在变化时发送（每天至多一次）：显卡厂商和驱动版本、Windows 版本号、屏幕数量、最高刷新率、正在使用的网卡是有线还是无线及其速度档。 | 否：厂商词、版本号、数量和档位。没有设备名、序列号和地址。 | 只保留厂商词、驱动和系统版本号里的数字；刷新率取最接近的标准值，网卡速度只取档位 `src/telemetry/telemetry.cpp:219` `src/telemetry/events.cpp:649`。 | 计数 400 天。 |
| svc_lifecycle：launched、adopted、extras_ended、exits、unexpected、soon_after_adopt、restarts_core_exit、restarts_session | `1`, `1`, `1`, `1`, `1`, `1`, `1`, `0` | `src/telemetry/events.cpp:623` `tools/sunshinesvc_lifecycle.h:54` | 服务对核心做了什么？服务留下一个小文件（核心怎样被启动或接管、多余的核心被结束、没有被要求就结束的核心、运行了多久）；主机下次启动时读出、上报计数并删除。 | 否：计数。文件里只有数字和固定词，没有路径和名称。 | 计数限幅，文件逐行按词表和范围检查 `src/telemetry/events.cpp:623` `src/telemetry/service_lifecycle.cpp:176`。 | 计数 400 天。 |
| startup_timing：which、ms、slow | `svc_core`, `34000`, `true` | `src/telemetry/events.cpp:633` `src/telemetry/service_lifecycle.cpp:176` | 打开到可以使用要多久？从服务启动到核心的网页界面可用（svc_core），以及页面从打开到可用的标记（page）；慢指超过 15 秒或远高于这台电脑自己的常见耗时。 | 否：固定词、毫秒数和是否。这台电脑自己的常见耗时保存在它自己的文件夹里。 | 固定词，数字限幅 `src/telemetry/events.cpp:633`。 | 计数 400 天。 |
| host_anomaly：code、severe、value、threshold、count、cause、which、exit_code | `XP_SVC_CORE_RESTART`, `true`, `30000`, `120000`, `1`, `soon_after_adopt` | `src/telemetry/events.cpp:638` `src/telemetry/service_lifecycle.cpp:176` | 核心有没有在没被要求时结束并重新启动（XP_SVC_CORE_RESTART），尤其是服务接管一个已在运行的核心后不久？启动是否很慢（XP_SLOW_READY）？重复出现或发生在接管之后：优先级 0。 | 否：两个固定代码之一、数字和固定词；退出码是一个数字。 | 固定代码和原因，数字限幅 `src/telemetry/events.cpp:638`。 | 计数 400 天。 |

## 错误事件（错误报告）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| error：component、code、severity、count | `stream`, `e1a2b3c4`, `error`, `3` | `src/telemetry/events.cpp:335` `src/telemetry/service.cpp:749` | 哪个组件出错、出错多频繁。code 是日志行固定开头（到第一个引号、括号、冒号或数字为止）的哈希，所以同一个错误是同一个 code。 | 否：不含文字。修复前，错误事件带一行最长 200 字符、经过清洗的日志，仍可能含设备名或游戏名；现在这行日志留在本机。 | 发送端白名单 `src/telemetry/events.cpp:335`、网页端 `m3e:src/telemetry/events.js:57` `m3e:src/telemetry/send.js:181`、Worker `worker:src/schema.ts:61` 都去掉了 message 属性；仍发送它的旧客户端，该属性会被丢弃。 | events 180 天。 |
| update_result：source、stage、result、error_class、from_version、to_version、dur_ms、reboot_needed | `mirror`, `download`, `failed`, `http_5xx` | `src/telemetry/events.cpp:592` | 更新的哪一步失败、来自哪个来源、是什么类别的原因？每个结束的步骤一条。 | 否：固定词（来源、步骤、结果、原因类别）、版本号和耗时。从不发送错误文字。 | 固定词，版本号只留数字和点 `src/telemetry/events.cpp:592`。 | 计数 400 天。 |
| install_result：result、error_class、phase、component、step、exit_code、reboot_needed、from_version、dur_ms | `failed`, `other`, `upgrade`, `usbip`, `roothub_guard`, `102` | `src/telemetry/events.cpp:602` `src/telemetry/install_journal.cpp:91` | 安装或升级的哪个组件在哪一步失败、退出码是什么、从哪个版本升级？安装器留下一份记录，主机下次启动时读取、上报并删除。 | 否：固定列表里的组件词和步骤词、退出码、版本号和耗时。记录里没有路径和错误文字。 | 逐行按词表和范围检查，不认识的组件被丢弃 `src/telemetry/install_journal.cpp:91` `src/telemetry/events.cpp:602`。 | 计数 400 天。 |
| install_summary：phase、result、components、failed、reboot_needed、from_version、dur_ms | `upgrade`, `failed`, `5`, `1` | `src/telemetry/events.cpp:613` `src/telemetry/install_journal.cpp:91` | 安装有没有完成？每次运行一条：成功、失败、取消，或「中断」（开始后一直没有结束，记录已安静十分钟）。 | 否：固定词和计数。 | 固定词，数字限幅 `src/telemetry/events.cpp:613`。 | 计数 400 天。 |
| error_report：component、lang、fingerprint、code、severity、loc、frames、crumbs、log、build、page、count | `host`, `cpp`, `a1b2c3d4e5f60789`, `E_ENCODER_INIT`, `src/video.cpp:4821:encoder_init`, `["run@a.js:1:2"]`, `["12\|nav\|apps"]`, `["encoder initialization failed"]` | `src/telemetry/events.cpp` `src/telemetry/error_report.cpp` `src/telemetry/service.cpp` `worker:src/errorreports.ts` | 错误发生在程序的哪个位置，以及出错前用户做了什么。同一个错误第一次立即发送，之后的只计数，每十分钟合并成一条数量发送；每小时最多 20 条。 | 低：位置和栈只有固定的几种形状（`模块+0x偏移`、我们自己源码树里的 `src/…:行:函数`、页面的 `函数@文件名:行:列`，只含文件名）；绝对路径、盘符、用户文件夹整条丢弃。面包屑是我们自己的 id，最近 50 个动作（页面切换、带标记的按钮、设置、串流开始和停止；没有文字）。`log` 是**组件自己日志的最后至多 40 行**，每行至多 200 个字符：它仍然是文本，不是固定词表。 | 组件先把日志匿名处理（Panel：`log_scrub.rs`），主机再用日志规则清洗每一行（名称、文件夹、包含 IPv6 在内的地址、邮箱、密钥；引号、方括号和花括号里的文字被丢弃）。仅在错误报告开启时、随一个错误的第一条报告发送（之后的没有栈和日志）。错误报告关闭期间不记录动作，关闭时已保留的内容被丢弃。 | 计数 400 天；一个错误分组的第一份样本（栈、面包屑、日志行）随分组保留 400 天。 |
| wer_event：event_id、kind、module、exception_code、offset | `1000`, `crash`, `zakovdd.dll`, `c0000005`, `0x1a2b3c` | `src/telemetry/wer_event.cpp` `src/telemetry/events.cpp` | 虚拟显示器驱动有没有出过故障？Windows 把它写在应用程序日志里，主机启动时读取。 | 否：只取故障模块是驱动自己文件的那一条，带异常代码和偏移；没有栈，也不索取；其他程序的事件数据从不复制。 | 固定形状和模块名。仅在错误报告开启时发送。 | events 180 天。 |
| log_summary：component、fatal、error、warning | `encoder`, `0`, `2`, `9` | `src/telemetry/events.cpp:351` `src/telemetry/service.cpp:749` | 主机每小时按组件记录了多少警告和错误。 | 否：计数。组件是 24 个固定词之一，或 `host`。 | `src/telemetry/service.cpp:749` | events 180 天。 |
| user_report：category、component、page、error_code、attachment、message | `bug`, `web`, `apps`, `E_LOST`, `att-2026-11/<sha256>.webp`, `the apps page stays empty` | `src/telemetry/events.cpp:343` `m3e:src/telemetry/report.js:140` `m3e:src/telemetry/report.js:127` | 用户亲手写、亲手点击「发送」的报告，回答「这个人遇到了什么问题」。报告会附上本次运行日志的引用 `log_ref`：日志按和崩溃报告相同的方式在本机匿名处理后上传（`POST /v1/user-log`），表单发送前说明会附上日志。 | 是，这是它的性质：它是人打的字，除它之外，其他文本是「从不发送」里列出的几种日志：随它附带的程序日志（`log_ref`）、严重串流问题之后的日志摘录和错误报告里的日志行。只有按下「发送」才会发出，且表单会先展示将发送的全部内容（标题、描述、错误的简短摘要及堆栈开头几行、最近的事件）。 | 在浏览器 `m3e:src/telemetry/scrub.js:52`、主机 `src/telemetry/events.cpp:154` 和 Worker `worker:src/scrub.ts:39` 各清洗一遍；最长 2000 字符；没有联系方式字段；附件只是开发者面板返回的引用。 | events 180 天；GitHub 问题在开发者关闭前保留。 |

## 崩溃报告（错误报告）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| meta：product、version、os_version、time、uptime_s | `host`, `2026.1112.0.杂鱼`, `10.0.26100` | `src/crash/crash_core.cpp:302` | 哪个版本崩溃、启动多久后崩溃。 | 否。 | 与信封相同的检查。 | crash_reports 400 天。 |
| meta：exception_code、module、module_offset、frames（至多 64 个） | `0xC0000005`, `sunshine.exe`, `0x1a2b3c` | `src/crash/crash_core.cpp:302` `src/crash/crash_core.cpp:231` `worker:src/crash.ts:24` | 崩溃发生在程序的哪个位置，以便对照该版本未剥离符号的二进制找到它。 | 否：文件名和我们自己二进制里的地址。目录在发送端 `src/crash/crash_core.cpp:231` 和 Worker `worker:src/crash.ts:24` 都会被去掉（路径里会有账号名）。 | 只留文件名；十六进制偏移；代码是固定形状。 | crash_signatures 400 天。 |
| signature | `ab…ab` (64 hex) | `src/crash/crash_core.cpp:302` | 把相同的崩溃归为一组。 | 否：前八帧的模块名和偏移的 SHA-256。 | — | crash_signatures 400 天。 |
| dump（主机、控制面板、sparxie-launch） | a file of tens to a few hundred KB | `src/crash/crash_win.cpp:186` `src/crash/crash_win.cpp:191` `src/crash/crash_core.cpp:506` `src/crash/crash_upload.cpp:98` `panel:src-tauri/src/crash/win.rs:210` `panel:src-tauri/src/crash/redact.rs:90` `tools/sparxie-launch/CrashReport.cs:302` `tools/sparxie-launch/DumpPrivacy.cs:87` | 让开发者能读到自己复现不了的崩溃的调用栈。 | 已压到最小。修复前，主机和控制面板请求了「间接引用内存」（每个栈指针指向的堆：TLS 私钥、配对密钥、Web UI 会话、剪贴板文字都可能在里面）。现在：没有堆、没有数据段、没有句柄数据，每个栈只保留指针大小的数值（MiniDumpFilterMemory），模块路径被过滤（MiniDumpFilterModulePaths），回调只保留模块名和 CodeView id、线程记录、上下文和栈。上传前，在本机把 `\Users\`、`/Users/`、`/home/` 后的账号名以及用户名、电脑名就地覆盖（ASCII 和 UTF-16）`src/crash/crash_core.cpp:506`。 | 转储类型和回调 `src/crash/crash_win.cpp:186`；字节级清洗有三种实现，使用同样的测试向量。 | 私有 GitHub 仓库的 release 资产，90 天后删除 `worker:wrangler.toml:31`；每个签名最多 20 份，每台安装每个签名一份 `worker:migrations/0001_init.sql:87`。 |
| 程序日志（主机的崩溃报告） | 崩溃那一次运行的完整日志，压缩后不超过 2 MiB；开头 64 KiB 和结尾 64 KiB 另以明文附上 | `src/crash/crash_log.cpp` `src/crash/log_scrub.cpp` `src/crash/crash_upload.cpp` | 让开发者看到崩溃前程序在做什么、报了什么错。 | 日志是程序自己写的文字，不是用户输入的；里面可能出现用户名、电脑名、路径、IP/MAC 地址、设备序列号、令牌、配对 PIN、Wi-Fi 名、游戏账号名，所以发出前先在本机匿名处理（见下）。 | 先在本机匿名处理：去掉用户名和电脑名、路径里的用户名、IPv4/IPv6/MAC（同一个值在同一份日志里是同一个占位符，如 `<ip#1>`，仍能前后对照）、邮箱、序列号和设备实例 ID、令牌/密钥/Cookie、配对 PIN、Wi-Fi 名、游戏账号名；时间、级别、组件、错误码、数字、版本、显卡和驱动字符串原样保留。服务端对开头和结尾的明文再检查一次，发现个人信息就整份拒收；之后在私有仓库里再整份清洗一遍，并用清洗后的副本替换。规则与测试向量见下面「怎样保证」。 | 随崩溃转储的发布资产保留 90 天（按月份发布，月底后再过 90 天删除）。 |
| meta：env（CPU 型号和线程数、显卡厂商/型号/驱动版本、内存、显示器数量、系统语言、安装方式） | `Intel(R) Core(TM) i9-13900K`, `32`, `nvidia`, `NVIDIA GeForce RTX 4090`, `32.0.15.6094`, `32768`, `2`, `zh-CN`, `installer` | `src/crash/crash_env.cpp` `worker:src/crashmeta.ts` | 崩溃集中在哪类机器、哪个显卡驱动上。 | 弱：硬件型号，不含电脑名、账号名、序列号。 | 只收固定词、数字和短的型号字符串，格式不符的丢弃（`src/crash/crash_env_text.cpp`、Worker 同一套规则）。 | crash_reports 400 天。 |
| meta：ctx（是否在串流、编码、分辨率、帧率、传输方式、客户端家族、HDR） | `true`, `hevc`, `3840x2160`, `120`, `altp`, `moonlight-macos` | `src/crash/crash_context.h` `src/crash/crash_core.cpp` | 崩溃时程序在做什么。 | 否：固定词和数字。 | 客户端家族是固定词之一，其余按类别写；不写设备名、客户端名。 | crash_reports 400 天。 |

## 服务器保存的数据

| 存放处 | 列 / 内容 | 代码位置 | 用途 | 保留时间 |
| --- | --- | --- | --- | --- |
| D1 activity | day, product, install_hash, version, channel, os, country, update_source | `worker:migrations/0001_init.sql:4` | 每天的用户数。 | 400 天 |
| D1 sessions | install_hash, session_id, day, product, version, channel, os, os_version, arch, gpu_vendor, locale, country, first_ms, last_ms, events, streams, stream_s, errors, exited, codec | `worker:migrations/0001_init.sql:16` | 每次运行一条摘要：时长、串流数、错误数、无崩溃比例。 | 400 天 |
| D1 events | day, t, install_hash, session_id, product, version, kind, name, props (JSON of the allow-listed props) | `worker:migrations/0001_init.sql:41` | 分位数、错误排行、页面出内容耗时。 | 180 天 |
| D1 crash_signatures | signature, version, product, kind, exception_code, module, module_offset, frames, first_ms, last_ms, count | `worker:migrations/0001_init.sql:55` | 崩溃分组、标出新的崩溃。 | 400 天 |
| D1 crash_reports | day, time_ms, install_hash, signature, version, product, kind, os_version, uptime_s, country | `worker:migrations/0001_init.sql:70` | 各版本崩溃率。 | 400 天 |
| D1 crash_dumps | signature, install_hash, tag, name, bytes, stored_ms (the dump itself is a release asset) | `worker:migrations/0001_init.sql:87` | 已保存转储的索引（转储本身是 release 资产）。 | 与转储一起 90 天 |
|  D1 crash_logs | signature, install_hash, tag, name, bytes, lines, stored_ms, version（日志本身是发布资产 `<签名前缀>-<哈希>.log.gz`） | `worker:migrations/0011_crash_detail.sql` | 已保存日志的索引。 同崩溃转储。 | 90 天。  |
|  D1 user_logs | ref, install_hash, bytes, lines, stored_ms, tail（日志最后 120 行，已匿名处理，最多 20000 字符） | `worker:migrations/0011_crash_detail.sql` | 问题反馈的 issue 里显示日志的最后几行。 同上。 | 14 天。  |
|  D1 crash_reports 的 env、ctx 列 | 见上面 env、ctx | `worker:migrations/0011_crash_detail.sql` | 同上。 同上。 | 400 天。  |
| D1 alerts, counters, report_state | alert key, title, body, labels, issue number; rate-limit counters; day of the daily report | `worker:migrations/0001_init.sql:98` `worker:migrations/0001_init.sql:114` | 提醒和报告去重；不超出免费额度。 | 7 到 400 天 |
| D1 page_hist, page_total, page_installs, api_hist, api_total | day, product, version, metric or endpoint, bucket, n (counts per fixed bucket); totals of visits and frames; the install hashes that sent a page summary on a day | `worker:src/pagevitals.ts:81` | 按版本看页面是否流畅、主机回答是否变慢。 | 400 天（安装哈希 35 天） |
| D1 install_daily, install_installs, install_runs, update_daily, device_dim | day, product, version, phase, component, step, result, class, exit code, upgrade-from version, n; the install hashes that met a failure (35 days); runs by outcome; update steps by source and stage; counts per device fact value | `worker:src/installevents.ts:43` | 按版本、组件、步骤和退出码看安装与更新的失败；哪类电脑有问题。 | 400 天（安装哈希 35 天） |
| D1 svc_total, ready_hist, hostanom_daily, hostanom_installs | day, product, version, counts of what the service did with the core, time-to-ready buckets by kind, anomaly counts by code, the install hashes that met an anomaly (35 days) | `worker:src/svclife.ts:34` | 按版本看核心是否自己结束、启动是否变慢，哪些安装遇到了异常。 | 400 天（安装哈希 35 天） |
| Analytics Engine | index1 = product; blobs: kind, name, version, channel, os, country, gpu_vendor, arch, install_hash and the string props; doubles: the numeric props | `worker:src/schema.ts:183` | 高频数据点的快速汇总。 | Cloudflare 的保留期 |
| GitHub issues (private repo) | daily report (totals and top lists), crash alerts (signature, product, version, exception, module, frames), user reports (category, product, version, channel, os, os_version, country, where, install hash prefix of 8 characters, time, message in a code fence) | `worker:src/report.ts:35` `worker:src/crash.ts:133` `worker:src/userreports.ts:17` | 负责人阅读报告和提醒。 | 直到关闭 |

## 从不发送

- 屏幕内容、声音、键盘输入、文件、剪贴板内容。
- 电脑名、Windows 或 macOS 账号名、设备名或客户端名；游戏名和应用名（游戏最多是九个目录词之一）。
- 文件路径、IP 地址、MAC 地址、端口、配对标识、Steam、米哈游或其他账号 id、Mirror酱 CDK、密码、令牌、Cookie。
- 未经匿名处理的程序日志。用户亲手写的报告文字 `user_report.message` 是人输入的自由文本，只在用户按下「发送」后发出，且表单事先展示过；程序自己写的日志是另一类文字，只随崩溃报告和问题反馈一起发送，并且先经过匿名处理（见上面「程序日志」一行），关闭错误报告后两者都不发送。
- 自由文本只有四处，除第一处外都仅在错误报告开启时发送（第一处需要用户按下「发送」）：（1）`user_report.message`：用户按下「发送」后发出，表单事先展示过；（2）随崩溃报告或反馈报告附带的程序日志（`user_report.log_ref`），在本机按上面「程序日志」一行的规则匿名处理（用户名和电脑名、路径里的用户名、IP 和 MAC 地址、邮箱、序列号、令牌、PIN、Wi-Fi 名、游戏账号名；引号里的名字除非属于这些类别，否则不会被这些规则去掉）；（3）严重串流问题之后的日志摘录（`stream_anomaly`，`log true`）：主机日志最近两分钟；（4）`error_report.log`：组件自己日志的最后至多 40 行（每行至多 200 个字符），随一个错误的第一条报告发送。（3）和（4）还会丢弃引号、方括号和花括号里的文字（只装代码的短方括号如 `[E12]` 保留），并清洗名称、文件夹、地址（IPv4 和 IPv6）、邮箱和密钥。四处都仍然是文本，不是固定词表，所以清洗后一行恰好写了什么就会被发送。其余都是固定词、数字或我们自己的 id。

## 怎样保证

- 发送端有白名单：未列出的事件名和属性在离开电脑前就被丢弃 `src/telemetry/events.cpp:335`（单元测试 `tests/unit/test_telemetry.cpp`）。
- Worker 对同一份名单再检查一次；格式不对的属性被丢弃，不会被「清洗后保存」`worker:src/schema.ts:61`。
- 三种语言的清洗规则用同一份测试向量：`tests/data/telemetry_scrub_vectors.json`（C++），m3e `test/scrub-vectors.json`（JS），Worker `test/scrub-vectors.json`（TS）；`scripts/telemetry/check-scrub-vectors.sh` 检查三份相同。向量里是虚构的用户名、电脑名、路径、IP、MAC、UUID、邮箱、令牌。
- 崩溃转储的字节级清洗在 C++、Rust、C# 各有一份实现和同样的测试向量。
- 程序日志的匿名处理有一份共用的测试向量：`tests/data/log_scrub_vectors.json`（C++，`tests/unit/test_log_scrub.cpp`）、Worker `test/log-scrub-vectors.json`（TS，参考实现）、私有仓库里解码工作流的 Python 版；向量里是虚构的用户名、电脑名、路径、IP、MAC、序列号、令牌、PIN、Wi-Fi 名、账号名，每一类都有。客户端先处理；服务端只抽查开头和结尾各 64 KiB，发现个人信息整份拒收；解码工作流在显示任何一行之前再整份清洗，并替换存储的文件。
- 同意：错误报告和使用统计各自独立；主机在每一次发送前都检查，网页发来的事件由主机再检查一次 `src/telemetry/service.cpp:228`。
- 用户可随时在「关于 › 隐私」看到「会发送什么」，并用「查看将发送的内容」看到下一次发送的原文 `src/telemetry/telemetry.cpp:470`。
