# 隐私数据清单：每个客户端发送什么、服务器保存什么

这份清单回答三个问题：发送了什么、为什么需要、怎样去除了个人信息。每一条都标出了代码位置（`文件:行号`）；代码位置由 `scripts/telemetry/build_inventory.py` 在生成时查找，所以会随代码更新。

## 一句话

两个独立的开关（错误报告、使用统计），默认开启，关闭后该类内容不再发送。数据只发往开发者自己的服务器 `api.alkaidlab.com`（Cloudflare Worker），不经过广告网络，不出售。除用户亲手写的反馈外，没有任何自由文本；名称、路径、地址、账号在发送前就不在载荷里。

服务器自己不保存 IP 地址，只保存 Cloudflare 给出的国家代码；Workers 日志已关闭 `worker:wrangler.toml:12`。连接本身必然经过网络，Cloudflare 作为网络服务商在传输层看得到连接的 IP，这一层不由我们控制，我们的代码不读取、不保存、不记录它。

## 每个批次都带的信封

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| install_id | `3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11` | `src/crash/crash_core.cpp:609` `src/crash/crash_core.cpp:593` | 统计日/周/月活跃安装数；把同一台安装的错误归在一起。 | 否：安装时随机生成的 UUID v4，与硬件、名称、账号无关。 | 服务器只存 install_hash（SHA-256 的前 32 位十六进制）`worker:src/crypto.ts:19`，不存原值。 | activity、sessions 400 天；events 180 天。 |
| session_id | `9a1c6b0e-52d4-4f1a-8e3b-7c2d5e9f0a13` | `src/telemetry/events.cpp:612` | 区分程序的每一次运行（计算无崩溃会话比例）。 | 否：每次启动重新随机生成。 | 随会话摘要保存。 | sessions 400 天。 |
| product, channel, os, arch | `host`, `stable`, `windows`, `x64` | `src/telemetry/events.cpp:612` `src/telemetry/telemetry.cpp:96` | 按产品、发布通道、系统和 CPU 架构拆分所有数字。 | 否：固定词。 | 发送端和 Worker 都用固定词表 `worker:src/schema.ts:165`。 | 随所在记录。 |
| version | `2026.1112.0.杂鱼` | `src/telemetry/events.cpp:588` | 知道哪个版本出了问题。 | 否：程序自己的版本号。 | 只允许字母、数字和 `. _ + -` 空格，其余置空 `src/telemetry/events.cpp:588`；Worker 用同一规则 `worker:src/schema.ts:165`。 | 随所在记录。 |
| os_version | `10.0.26100` | `src/telemetry/telemetry.cpp:211` `src/telemetry/events.cpp:597` | 发现只在某个 Windows 版本上出现的问题。 | 否：系统版本号，数百万人相同。 | 只允许字母、数字和 `. _ + -` 空格，最长 48 字符 `src/telemetry/events.cpp:597` `worker:src/schema.ts:165`；不符合的不再「清洗后发送」，而是直接丢弃。 | sessions 400 天。 |
| gpu_vendor | `nvidia` | `src/telemetry/telemetry.cpp:125` | 发现只在某个显卡厂商上出现的问题。 | 否：四个词之一（nvidia、amd、intel、apple）或 `other`；不含显卡型号和驱动字符串。 | 厂商先被归为一个词 `src/telemetry/telemetry.cpp:125`；Worker 只接受短的小写标记。 | sessions 400 天。 |
| locale | `zh-CN` | `src/telemetry/events.cpp:604` | 知道哪些语言需要优先照顾。 | 否：语言标签。 | 必须符合语言标签格式，否则置空 `src/telemetry/events.cpp:604` `worker:src/schema.ts:165`。 | sessions 400 天。 |
| country（服务器添加） | `CN` | `worker:src/http.ts:67` | 了解用户所在地区，用于语言和网络方面的决策。 | 弱：只到国家，不到城市和地址。 | 只保留 Cloudflare 给出的国家代码；Worker 不读取、不保存、不记录 IP 地址；Workers 日志已关闭 `worker:wrangler.toml:12`。 | sessions、activity、crash_reports 400 天。 |

## 使用事件（使用统计）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| app_start：cold、start_ms、update_source | `true`, `1800`, `mirror` | `src/telemetry/events.cpp:301` | 启动耗时；更新来源（Mirror酱还是 GitHub）。 | 否：一个布尔值、一个数字、三个词之一。 | 按类型和范围的白名单 `src/telemetry/events.cpp:301`。 | events 180 天。 |
| app_exit：uptime_s | `3600` | `src/telemetry/events.cpp:302` | 程序运行多久；会话是否正常结束。 | 否：一个数字。 | 范围 0 到一年。 | events 180 天。 |
| stream_start / stream_end：client_kind | `moonlight-macos-enhanced` | `src/telemetry/events.cpp:303` `src/telemetry/stream_stats.cpp:92` | 哪些客户端家族在使用，用来决定投入方向。 | 否：只能是主机自己识别的六个家族之一，否则为 `other`。用户给设备起的名字不会传出（修复前这是一个自由的 catalogue id）。 | 发送端白名单 `src/telemetry/stream_stats.cpp:92`，Worker 端枚举 `worker:src/schema.ts:53`。 | events 180 天。 |
| stream_start / stream_end：client_os、codec、width、height、fps、hdr、audio_codec、transport | `macos`, `hevc`, `2560`, `1440`, `120`, `true`, `opus`, `altp` | `src/telemetry/events.cpp:303` `src/telemetry/stream_stats.cpp:122` | 人们串流用什么格式和分辨率；ALTP 传输用得多不多。 | 否：固定词和数字。 | 发送端 `src/telemetry/events.cpp:303` 和 Worker `worker:src/schema.ts:53` 都用枚举；数字被限幅。 | events 180 天。 |
| stream_start / stream_end：app_kind | `sparxie` | `src/telemetry/stream_stats.cpp:56` | 桌面、大屏、Sparxie、Steam 或其他：哪类使用最重要。 | 否：五个词之一。应用名称和命令只用来选词，不会发送。 | `src/telemetry/stream_stats.cpp:56` | events 180 天。 |
| stream_start / stream_end：sparxie_game | `genshin` | `src/telemetry/stream_stats.cpp:102` | 哪些 Sparxie 游戏在使用（只用目录里的 id）。 | 否：Sparxie 目录里的九个 id 之一，或 `other`；手写或未知的值变成 `other`。 | `src/telemetry/stream_stats.cpp:102`，Worker 端枚举 `worker:src/schema.ts:53`。 | events 180 天。 |
| stream_end：duration_s、result | `600`, `normal` | `src/telemetry/events.cpp:307` | 串流总时长（「串流分钟数」的来源）；串流以什么方式结束。 | 否：一个数字和四个词之一。 | `src/telemetry/events.cpp:307` | events 180 天；sessions 保存合计 400 天。 |
| feature_used：feature | `stream.hdr`, `web.app_added`, `panel.pair_open` | `src/telemetry/events.cpp:308` `src/telemetry/service.cpp:220` | 哪些功能真的有人用。 | 否：我们自己的 id。网页只能用自己产品前缀（`web.` 或 `panel.`）的 id `src/telemetry/service.cpp:220`。 | 小写标记 `src/telemetry/events.cpp:308`；网页事件的前缀检查 `src/telemetry/service.cpp:220`。 | events 180 天。 |
| setting_changed：key、value | `max_bitrate`, `15000-30000` | `src/telemetry/events.cpp:309` `src/telemetry/events.cpp:460` | 人们改了哪些设置、改成什么类型的值。 | 否：key 来自 27 项名单；value 是名单里的词或区间。路径、名称、地址这类自由值不在名单里。 | `src/telemetry/events.cpp:460`，Worker 重复检查名单和取值格式 `worker:src/schema.ts:61`。 | events 180 天。 |

## 体验事件（使用统计）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| stream_quality（每次串流一条） | `fps_p50 119`, `encode_ms_p95 6`, `loss_pct 0.1`, `reconnects 0` | `src/telemetry/events.cpp:332` | 串流是否流畅？按版本和编码看帧率、延迟、丢包、卡顿。 | 否：数字。 | 限幅并四舍五入到一位小数 `src/telemetry/events.cpp:332`。 | events 180 天；AE 数据点按 Cloudflare 的保留期。 |
| startup：host_start_ms、first_frame_ms | `2100`, `450` | `src/telemetry/events.cpp:351` | 主机多久就绪、多久出第一帧。 | 否：数字。 | 限幅。 | events 180 天。 |
| panel_page：page、time_to_content_ms | `apps`, `180` | `src/telemetry/events.cpp:352` | 控制面板各页面多久显示出内容。 | 否：我们自己的路由 id 和一个数字。 | 标记和范围。 | events 180 天。 |

## 错误事件（错误报告）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| error：component、code、severity、count | `stream`, `e1a2b3c4`, `error`, `3` | `src/telemetry/events.cpp:310` `src/telemetry/service.cpp:393` | 哪个组件出错、出错多频繁。code 是日志行固定开头（到第一个引号、括号、冒号或数字为止）的哈希，所以同一个错误是同一个 code。 | 否：不含文字。修复前，错误事件带一行最长 200 字符、经过清洗的日志，仍可能含设备名或游戏名；现在这行日志留在本机。 | 发送端白名单 `src/telemetry/events.cpp:310`、网页端 `m3e:src/telemetry/events.js:57` `m3e:src/telemetry/send.js:181`、Worker `worker:src/schema.ts:61` 都去掉了 message 属性；仍发送它的旧客户端，该属性会被丢弃。 | events 180 天。 |
| log_summary：component、fatal、error、warning | `encoder`, `0`, `2`, `9` | `src/telemetry/events.cpp:326` `src/telemetry/service.cpp:393` | 主机每小时按组件记录了多少警告和错误。 | 否：计数。组件是 24 个固定词之一，或 `host`。 | `src/telemetry/service.cpp:393` | events 180 天。 |
| user_report：category、component、page、error_code、attachment、message | `bug`, `web`, `apps`, `E_LOST`, `att-2026-11/<sha256>.webp`, `the apps page stays empty` | `src/telemetry/events.cpp:318` `m3e:src/telemetry/report.js:140` `m3e:src/telemetry/report.js:127` | 用户亲手写、亲手点击「发送」的报告，回答「这个人遇到了什么问题」。 | 是，这是它的性质：它是人打的字，也是整个系统里唯一的自由文本。只有按下「发送」才会发出，且表单会先展示将发送的全部内容（标题、描述、错误的简短摘要及堆栈开头几行、最近的事件）。 | 在浏览器 `m3e:src/telemetry/scrub.js:52`、主机 `src/telemetry/events.cpp:153` 和 Worker `worker:src/scrub.ts:39` 各清洗一遍；最长 2000 字符；没有联系方式字段；附件只是开发者面板返回的引用。 | events 180 天；GitHub 问题在开发者关闭前保留。 |

## 崩溃报告（错误报告）

| 字段 | 示例 | 代码位置 | 用途（回答什么问题） | 能否识别个人或泄露私密内容 | 已做的清洗 | 保留时间 |
| --- | --- | --- | --- | --- | --- | --- |
| meta：product、version、os_version、time、uptime_s | `host`, `2026.1112.0.杂鱼`, `10.0.26100` | `src/crash/crash_core.cpp:302` | 哪个版本崩溃、启动多久后崩溃。 | 否。 | 与信封相同的检查。 | crash_reports 400 天。 |
| meta：exception_code、module、module_offset、frames（至多 64 个） | `0xC0000005`, `sunshine.exe`, `0x1a2b3c` | `src/crash/crash_core.cpp:302` `src/crash/crash_core.cpp:231` `worker:src/crash.ts:24` | 崩溃发生在程序的哪个位置，以便对照该版本未剥离符号的二进制找到它。 | 否：文件名和我们自己二进制里的地址。目录在发送端 `src/crash/crash_core.cpp:231` 和 Worker `worker:src/crash.ts:24` 都会被去掉（路径里会有账号名）。 | 只留文件名；十六进制偏移；代码是固定形状。 | crash_signatures 400 天。 |
| signature | `ab…ab` (64 hex) | `src/crash/crash_core.cpp:302` | 把相同的崩溃归为一组。 | 否：前八帧的模块名和偏移的 SHA-256。 | — | crash_signatures 400 天。 |
| dump（主机、控制面板、sparxie-launch） | a file of tens to a few hundred KB | `src/crash/crash_win.cpp:186` `src/crash/crash_win.cpp:191` `src/crash/crash_core.cpp:506` `src/crash/crash_upload.cpp:98` `panel:src-tauri/src/crash/win.rs:210` `panel:src-tauri/src/crash/redact.rs:90` `tools/sparxie-launch/CrashReport.cs:302` `tools/sparxie-launch/DumpPrivacy.cs:87` | 让开发者能读到自己复现不了的崩溃的调用栈。 | 已压到最小。修复前，主机和控制面板请求了「间接引用内存」（每个栈指针指向的堆：TLS 私钥、配对密钥、Web UI 会话、剪贴板文字都可能在里面）。现在：没有堆、没有数据段、没有句柄数据，每个栈只保留指针大小的数值（MiniDumpFilterMemory），模块路径被过滤（MiniDumpFilterModulePaths），回调只保留模块名和 CodeView id、线程记录、上下文和栈。上传前，在本机把 `\Users\`、`/Users/`、`/home/` 后的账号名以及用户名、电脑名就地覆盖（ASCII 和 UTF-16）`src/crash/crash_core.cpp:506`。 | 转储类型和回调 `src/crash/crash_win.cpp:186`；字节级清洗有三种实现，使用同样的测试向量。 | 私有 GitHub 仓库的 release 资产，90 天后删除 `worker:wrangler.toml:31`；每个签名最多 20 份，每台安装每个签名一份 `worker:migrations/0001_init.sql:87`。 |

## 服务器保存的数据

| 存放处 | 列 / 内容 | 代码位置 | 用途 | 保留时间 |
| --- | --- | --- | --- | --- |
| D1 activity | day, product, install_hash, version, channel, os, country, update_source | `worker:migrations/0001_init.sql:4` | 每天的用户数。 | 400 天 |
| D1 sessions | install_hash, session_id, day, product, version, channel, os, os_version, arch, gpu_vendor, locale, country, first_ms, last_ms, events, streams, stream_s, errors, exited, codec | `worker:migrations/0001_init.sql:16` | 每次运行一条摘要：时长、串流数、错误数、无崩溃比例。 | 400 天 |
| D1 events | day, t, install_hash, session_id, product, version, kind, name, props (JSON of the allow-listed props) | `worker:migrations/0001_init.sql:41` | 分位数、错误排行、页面出内容耗时。 | 180 天 |
| D1 crash_signatures | signature, version, product, kind, exception_code, module, module_offset, frames, first_ms, last_ms, count | `worker:migrations/0001_init.sql:55` | 崩溃分组、标出新的崩溃。 | 400 天 |
| D1 crash_reports | day, time_ms, install_hash, signature, version, product, kind, os_version, uptime_s, country | `worker:migrations/0001_init.sql:70` | 各版本崩溃率。 | 400 天 |
| D1 crash_dumps | signature, install_hash, tag, name, bytes, stored_ms (the dump itself is a release asset) | `worker:migrations/0001_init.sql:87` | 已保存转储的索引（转储本身是 release 资产）。 | 与转储一起 90 天 |
| D1 alerts, counters, report_state | alert key, title, body, labels, issue number; rate-limit counters; day of the daily report | `worker:migrations/0001_init.sql:98` `worker:migrations/0001_init.sql:114` | 提醒和报告去重；不超出免费额度。 | 7 到 400 天 |
| Analytics Engine | index1 = product; blobs: kind, name, version, channel, os, country, gpu_vendor, arch, install_hash and the string props; doubles: the numeric props | `worker:src/schema.ts:183` | 高频数据点的快速汇总。 | Cloudflare 的保留期 |
| GitHub issues (private repo) | daily report (totals and top lists), crash alerts (signature, product, version, exception, module, frames), user reports (category, product, version, channel, os, os_version, country, where, install hash prefix of 8 characters, time, message in a code fence) | `worker:src/report.ts:35` `worker:src/crash.ts:133` `worker:src/userreports.ts:17` | 负责人阅读报告和提醒。 | 直到关闭 |

## 从不发送

- 屏幕内容、声音、键盘输入、文件、剪贴板内容。
- 电脑名、Windows 或 macOS 账号名、设备名或客户端名；游戏名和应用名（游戏最多是九个目录词之一）。
- 文件路径、IP 地址、MAC 地址、端口、配对标识、Steam、米哈游或其他账号 id、Mirror酱 CDK、密码、令牌、Cookie。
- 任何不是用户为了写报告而输入的自由文本：整个系统里唯一的自由文本是 `user_report.message`，只在用户按下「发送」后发出，且表单事先展示过。

## 怎样保证

- 发送端有白名单：未列出的事件名和属性在离开电脑前就被丢弃 `src/telemetry/events.cpp:310`（单元测试 `tests/unit/test_telemetry.cpp`）。
- Worker 对同一份名单再检查一次；格式不对的属性被丢弃，不会被「清洗后保存」`worker:src/schema.ts:61`。
- 三种语言的清洗规则用同一份测试向量：`tests/data/telemetry_scrub_vectors.json`（C++），m3e `test/scrub-vectors.json`（JS），Worker `test/scrub-vectors.json`（TS）；`scripts/telemetry/check-scrub-vectors.sh` 检查三份相同。向量里是虚构的用户名、电脑名、路径、IP、MAC、UUID、邮箱、令牌。
- 崩溃转储的字节级清洗在 C++、Rust、C# 各有一份实现和同样的测试向量。
- 同意：错误报告和使用统计各自独立；主机在每一次发送前都检查，网页发来的事件由主机再检查一次 `src/telemetry/service.cpp:220`。
- 用户可随时在「关于 › 隐私」看到「会发送什么」，并用「查看将发送的内容」看到下一次发送的原文 `src/telemetry/telemetry.cpp:470`。
