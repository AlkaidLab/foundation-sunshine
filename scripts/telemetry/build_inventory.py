#!/usr/bin/env python3
"""Builds docs/privacy-data-inventory.md and docs/privacy-data-inventory.zh.md in the host repo from one table.
NOTE: since #1274 the two md files carry rows that this script does not have (the program log of crash reports, meta env and ctx, D1 crash_logs and user_logs, the log_ref of user_report). Regenerating overwrites them:
extend this script with those rows first, or merge the output by hand. The `src/telemetry/events.cpp:LINE` references of the two files are kept right by `inventory_refs.py` (run it after changing events.cpp; `--check` lists what is off).
Every `@name` in a cell is replaced by `path:line` found by searching the code for the anchor (so the evidence moves with the code)."""
import re, subprocess, sys

import os
HOST = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")) + "/"
# the sibling repositories (the UI library, the control panel and the telemetry Worker); override with the environment when they live elsewhere
M3E = os.environ.get("ALK_M3E", "/scratch/tp-m3e") + "/"
PANEL = os.environ.get("ALK_PANEL", "/scratch/tp-panel") + "/"
WORKER = os.environ.get("ALK_WORKER", os.path.expanduser("~/Projects/alkaidlab-telemetry-worker")) + "/"

# anchor name -> (root, file, regex that starts the thing)
A = {
    "ctx": (HOST, "src/telemetry/events.cpp", r"^  envelope_fields\("),
    "safever": (HOST, "src/telemetry/events.cpp", r"^  safe_version\("),
    "safeos": (HOST, "src/telemetry/events.cpp", r"^  safe_os_version\("),
    "safeloc": (HOST, "src/telemetry/events.cpp", r"^  safe_locale\("),
    "fillctx": (HOST, "src/telemetry/telemetry.cpp", r"fill_context\(\) \{"),
    "gpuvendor": (HOST, "src/telemetry/telemetry.cpp", r"gpu_vendor_of\(std::string_view"),
    "channel": (HOST, "src/telemetry/telemetry.cpp", r"channel_name\(\) \{"),
    "installid": (HOST, "src/crash/crash_core.cpp", r"^  read_or_create_install_id\("),
    "uuid4": (HOST, "src/crash/crash_core.cpp", r"^  new_uuid_v4\("),
    "scrub": (HOST, "src/telemetry/events.cpp", r"^  scrub_text\("),
    "scruglog": (HOST, "src/telemetry/events.cpp", r"^  scrub_log_message\("),
    "e_app_start": (HOST, "src/telemetry/events.cpp", r'"app_start", \{'),
    "e_app_exit": (HOST, "src/telemetry/events.cpp", r'"app_exit", \{'),
    "e_stream_start": (HOST, "src/telemetry/events.cpp", r'"stream_start", stream_dims'),
    "e_stream_end": (HOST, "src/telemetry/events.cpp", r't.push_back\(\{ kind_e::usage, "stream_end", end'),
    "e_feature": (HOST, "src/telemetry/events.cpp", r'"feature_used", \{'),
    "e_setting": (HOST, "src/telemetry/events.cpp", r'"setting_changed", \{'),
    "e_error": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "error", \{'),
    "e_user_report": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "user_report", \{'),
    "e_log_summary": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "log_summary", \{'),
    "e_stream_quality": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "stream_quality", \{'),
    "e_stream_health": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "stream_health", std::move'),
    "e_stream_anomaly": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "stream_anomaly", std::move'),
    "e_stream_regress": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "stream_regress", std::move'),
    "e_startup": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "startup", \{'),
    "e_panel_page": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "panel_page", \{'),
    "e_page_vitals": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "page_vitals", \{'),
    "e_api_latency": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "api_latency", \{'),
    "e_update_result": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "update_result", \{'),
    "e_install_result": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "install_result", \{'),
    "e_install_summary": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "install_summary", \{'),
    "e_error_report": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "error_report", \{'),
    "e_wer_event": (HOST, "src/telemetry/events.cpp", r'kind_e::error, "wer_event", \{'),
    "e_device_facts": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "device_facts", \{'),
    "e_svc_lifecycle": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "svc_lifecycle", \{'),
    "e_startup_timing": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "startup_timing", \{'),
    "e_host_anomaly": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "host_anomaly", \{'),
    "svcjudge": (HOST, "src/telemetry/service_lifecycle.cpp", r"^  judge_restart\("),
    "svcwrite": (HOST, "tools/sunshinesvc_lifecycle.h", r"core_exit_line\("),
    "w_svc": (WORKER, "src/svclife.ts", r"export function planSvcWrites"),
    "journal": (HOST, "src/telemetry/install_journal.cpp", r"^  events_from_journal\("),
    "facts": (HOST, "src/telemetry/telemetry.cpp", r"facts_from\(const system_info::snapshot_t"),
    "js_perf": (M3E, "src/telemetry/pagePerf.js", r"export function createPagePerf"),
    "js_buckets": (M3E, "src/telemetry/pageBuckets.js", r"export const PAGE_BUCKETS"),
    "w_page": (WORKER, "src/pagevitals.ts", r"export function planPageWrites"),
    "w_install": (WORKER, "src/installevents.ts", r"export function planInstallWrites"),
    "settings": (HOST, "src/telemetry/events.cpp", r"static const std::vector<setting_spec_t> table"),
    "ckind": (HOST, "src/telemetry/stream_stats.cpp", r"^  client_kind_of\("),
    "cos": (HOST, "src/telemetry/stream_stats.cpp", r"^  client_os_of\("),
    "game": (HOST, "src/telemetry/stream_stats.cpp", r"^  sparxie_game_id\("),
    "appkind": (HOST, "src/telemetry/stream_stats.cpp", r"^  classify_app_kind\("),
    "logs": (HOST, "src/telemetry/service.cpp", r"service_t::collect_logs"),
    "ingest": (HOST, "src/telemetry/service.cpp", r"service_t::ingest\("),
    "mayclient": (HOST, "src/telemetry/events.cpp", r"^  client_may_send\("),
    "consent": (HOST, "src/telemetry/service.cpp", r"consent"),
    "preview": (HOST, "src/telemetry/telemetry.cpp", r"^  preview\(\) \{"),
    "dumptype": (HOST, "src/crash/crash_win.cpp", r"constexpr int dump_type_small"),
    "dumpfilter": (HOST, "src/crash/crash_win.cpp", r"dump_filter\(PVOID"),
    "redact": (HOST, "src/crash/crash_core.cpp", r"^  redact_private_names\("),
    "privnames": (HOST, "src/crash/crash_upload.cpp", r"private_names\(\) \{"),
    "meta": (HOST, "src/crash/crash_core.cpp", r"^  render_meta\("),
    "basename": (HOST, "src/crash/crash_core.cpp", r"base_name\(const char"),
    "rustdump": (PANEL, "src-tauri/src/crash/win.rs", r"fn write_dump"),
    "rustredact": (PANEL, "src-tauri/src/crash/redact.rs", r"pub\(crate\) fn redact_private_names"),
    "rustpanic": (PANEL, "src-tauri/src/crash/mod.rs", r"fn on_panic"),
    "csdump": (HOST, "tools/sparxie-launch/CrashReport.cs", r"internal const int DumpType"),
    "csredact": (HOST, "tools/sparxie-launch/DumpPrivacy.cs", r"internal static int Redact"),
    "js_err": (M3E, "src/telemetry/events.js", r"const errorEvent ="),
    "js_issue": (M3E, "src/telemetry/send.js", r"export async function createIssue"),
    "js_report": (M3E, "src/telemetry/report.js", r"export function reportToEvent"),
    "js_msg": (M3E, "src/telemetry/report.js", r"export function reportMessage"),
    "js_scrub": (M3E, "src/telemetry/scrub.js", r"export function scrubText"),
    "w_schema": (WORKER, "src/schema.ts", r"export const EVENTS"),
    "w_env": (WORKER, "src/schema.ts", r"export function cleanEnvelope"),
    "w_dims": (WORKER, "src/schema.ts", r"const STREAM_DIMS"),
    "w_crash": (WORKER, "src/crash.ts", r"export function cleanCrashMeta"),
    "w_scrub": (WORKER, "src/scrub.ts", r"export function scrubText"),
    "w_activity": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE activity"),
    "w_sessions": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE sessions"),
    "w_events": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE events"),
    "w_sigs": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE crash_signatures"),
    "w_reports": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE crash_reports"),
    "w_dumps": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE crash_dumps"),
    "w_alerts": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE alerts"),
    "w_counters": (WORKER, "migrations/0001_init.sql", r"CREATE TABLE counters"),
    "w_ae": (WORKER, "src/schema.ts", r"export const AE_COMMON_BLOBS"),
    "w_hash": (WORKER, "src/crypto.ts", r"installHash"),
    "w_country": (WORKER, "src/http.ts", r"countryOf"),
    "w_sweep": (WORKER, "src/cron.ts", r"export async function sweep"),
    "w_obs": (WORKER, "wrangler.toml", r"^\[observability\]"),
    "w_retention": (WORKER, "wrangler.toml", r"^EVENT_RETENTION_DAYS"),
    "w_dumpret": (WORKER, "wrangler.toml", r"^DUMP_RETENTION_DAYS"),
    "w_userrep": (WORKER, "src/userreports.ts", r"export async function notifyUserReports"),
    "w_alertsrc": (WORKER, "src/crash.ts", r"if \(isNew && !isTestVersion"),
    "w_report": (WORKER, "src/report.ts", r"export async function buildReport"),
}

def where(name):
    root, rel, rx = A[name]
    pat = re.compile(rx)
    try:
        with open(root + rel, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if pat.search(line):
                    short = {HOST: "", M3E: "m3e:", PANEL: "panel:", WORKER: "worker:"}[root]
                    return f"`{short}{rel}:{n}`"
    except FileNotFoundError:
        pass
    sys.stderr.write(f"anchor not found: {name} ({rel} /{rx}/)\n")
    return f"`{rel}`"

def fill(text):
    return re.sub(r"@([a-z0-9_]+)", lambda m: where(m.group(1)) if m.group(1) in A else m.group(0), text)

# ---------------------------------------------------------------- the rows
# (field, example, source anchors, purpose en, identify? en, scrubbing en, retention en) and the zh texts after them
R = []

def row(group, field, example, src, purpose, risk, scrub, keep, zh):
    R.append(dict(group=group, field=field, example=example, src=src, purpose=purpose, risk=risk, scrub=scrub, keep=keep, zh=zh))

# --- envelope
row("envelope", "install_id", "`3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11`", "@installid @uuid4",
    "Count daily, weekly and monthly users; group one install's errors together.", "No: random UUID v4 made once, not derived from hardware, name or account.", "Stored only as install_hash (first 32 hex of SHA-256) @w_hash; the raw id is never stored.", "activity, sessions: 400 days; events: 180 days.",
    dict(field="install_id", purpose="统计日/周/月活跃安装数；把同一台安装的错误归在一起。", risk="否：安装时随机生成的 UUID v4，与硬件、名称、账号无关。", scrub="服务器只存 install_hash（SHA-256 的前 32 位十六进制）@w_hash，不存原值。", keep="activity、sessions 400 天；events 180 天。"))
row("envelope", "session_id", "`9a1c6b0e-52d4-4f1a-8e3b-7c2d5e9f0a13`", "@ctx",
    "Tell one run of the program from another (crash-free sessions).", "No: new random UUID at every start.", "Stored with the session summary.", "sessions: 400 days.",
    dict(field="session_id", purpose="区分程序的每一次运行（计算无崩溃会话比例）。", risk="否：每次启动重新随机生成。", scrub="随会话摘要保存。", keep="sessions 400 天。"))
row("envelope", "product, channel, os, arch", "`host`, `stable`, `windows`, `x64`", "@ctx @channel",
    "Split every number by product, release channel, OS and CPU.", "No: fixed words.", "Fixed lists in the sender and the Worker @w_env.", "As the record that carries it.",
    dict(field="product, channel, os, arch", purpose="按产品、发布通道、系统和 CPU 架构拆分所有数字。", risk="否：固定词。", scrub="发送端和 Worker 都用固定词表 @w_env。", keep="随所在记录。"))
row("envelope", "version", "`2026.1112.0.杂鱼`", "@safever",
    "Know which release has the problem.", "No: the program's own version.", "Only letters, digits and `. _ + -` space are allowed; anything else becomes empty @safever; the Worker applies the same pattern @w_env.", "As the record that carries it.",
    dict(field="version", purpose="知道哪个版本出了问题。", risk="否：程序自己的版本号。", scrub="只允许字母、数字和 `. _ + -` 空格，其余置空 @safever；Worker 用同一规则 @w_env。", keep="随所在记录。"))
row("envelope", "os_version", "`10.0.26100`", "@fillctx @safeos",
    "Find problems that only occur on one Windows build.", "No: the OS build number, shared by millions.", "Only letters, digits and `. _ + -` space, at most 48 characters @safeos @w_env; free text is no longer scrubbed and passed on, it is refused.", "sessions: 400 days.",
    dict(field="os_version", purpose="发现只在某个 Windows 版本上出现的问题。", risk="否：系统版本号，数百万人相同。", scrub="只允许字母、数字和 `. _ + -` 空格，最长 48 字符 @safeos @w_env；不符合的不再「清洗后发送」，而是直接丢弃。", keep="sessions 400 天。"))
row("envelope", "gpu_vendor", "`nvidia`", "@gpuvendor",
    "Find problems that only occur with one graphics vendor.", "No: one of four words (nvidia, amd, intel, apple) or `other`; no device name, no driver string.", "The vendor is reduced to a word @gpuvendor; the Worker takes only short lower-case tokens.", "sessions: 400 days.",
    dict(field="gpu_vendor", purpose="发现只在某个显卡厂商上出现的问题。", risk="否：四个词之一（nvidia、amd、intel、apple）或 `other`；不含显卡型号和驱动字符串。", scrub="厂商先被归为一个词 @gpuvendor；Worker 只接受短的小写标记。", keep="sessions 400 天。"))
row("envelope", "locale", "`zh-CN`", "@safeloc",
    "Know which languages need attention.", "No: a language tag.", "Must match a language-tag pattern, else empty @safeloc @w_env.", "sessions: 400 days.",
    dict(field="locale", purpose="知道哪些语言需要优先照顾。", risk="否：语言标签。", scrub="必须符合语言标签格式，否则置空 @safeloc @w_env。", keep="sessions 400 天。"))
row("envelope", "country (added by the server)", "`CN`", "@w_country",
    "Know where users are, for language and network decisions.", "Weak: a country, never a city or an address.", "Cloudflare's country code is the only place information kept; the IP address is not read, stored or logged by the Worker; Workers Logs are off @w_obs.", "sessions, activity, crash_reports: 400 days.",
    dict(field="country（服务器添加）", purpose="了解用户所在地区，用于语言和网络方面的决策。", risk="弱：只到国家，不到城市和地址。", scrub="只保留 Cloudflare 给出的国家代码；Worker 不读取、不保存、不记录 IP 地址；Workers 日志已关闭 @w_obs。", keep="sessions、activity、crash_reports 400 天。"))

# --- usage events
row("usage", "app_start: cold, start_ms, update_source, update_source_miss", "`true`, `1800`, `cnb`, `version_lag`", "@e_app_start",
    "How long start takes; where the last update really came from (Mirror酱, CNB or GitHub) and, when Mirror酱 was chosen but not used, why (not synced yet, CDK refused, quota, network, no CDK).", "No: a boolean, a number, one of four words, one of five words.", "Allow-list of types and ranges @e_app_start.", "events: 180 days.",
    dict(field="app_start：cold、start_ms、update_source、update_source_miss", purpose="启动耗时；最近一次更新实际来自哪里（Mirror酱、CNB 或 GitHub），选了 Mirror酱 却没有用上时的原因（还没同步该版本、CDK 被拒、额度、网络、没有 CDK）。", risk="否：一个布尔值、一个数字、四个词之一、五个词之一。", scrub="按类型和范围的白名单 @e_app_start。", keep="events 180 天。"))
row("usage", "app_exit: uptime_s", "`3600`", "@e_app_exit",
    "How long the program runs; whether a session ended normally.", "No: a number.", "Range 0 to one year.", "events: 180 days.",
    dict(field="app_exit：uptime_s", purpose="程序运行多久；会话是否正常结束。", risk="否：一个数字。", scrub="范围 0 到一年。", keep="events 180 天。"))
row("usage", "stream_start / stream_end: client_kind", "`moonlight-macos-enhanced`", "@e_stream_start @ckind",
    "Which client families are used, to decide where to invest.", "No: one of the six families the host itself recognises, else `other`. A name a person gave a device is never passed (this was a free catalogue id before).", "Allow-list in the sender @ckind and an enum in the Worker @w_dims.", "events: 180 days.",
    dict(field="stream_start / stream_end：client_kind", purpose="哪些客户端家族在使用，用来决定投入方向。", risk="否：只能是主机自己识别的六个家族之一，否则为 `other`。用户给设备起的名字不会传出（修复前这是一个自由的 catalogue id）。", scrub="发送端白名单 @ckind，Worker 端枚举 @w_dims。", keep="events 180 天。"))
row("usage", "stream_start / stream_end: client_os, codec, width, height, fps, hdr, audio_codec, transport", "`macos`, `hevc`, `2560`, `1440`, `120`, `true`, `opus`, `altp`", "@e_stream_start @cos",
    "Which formats and resolutions people stream; whether the ALTP transport is used.", "No: fixed words and numbers.", "Enums in the sender @e_stream_start and in the Worker @w_dims; numbers clamped.", "events: 180 days.",
    dict(field="stream_start / stream_end：client_os、codec、width、height、fps、hdr、audio_codec、transport", purpose="人们串流用什么格式和分辨率；ALTP 传输用得多不多。", risk="否：固定词和数字。", scrub="发送端 @e_stream_start 和 Worker @w_dims 都用枚举；数字被限幅。", keep="events 180 天。"))
row("usage", "stream_start / stream_end: app_kind", "`sparxie`", "@appkind",
    "Desktop, big-screen, Sparxie, Steam or other: which kinds of use matter.", "No: five words. The application's name and command are read to choose the word and are not sent.", "@appkind", "events: 180 days.",
    dict(field="stream_start / stream_end：app_kind", purpose="桌面、大屏、Sparxie、Steam 或其他：哪类使用最重要。", risk="否：五个词之一。应用名称和命令只用来选词，不会发送。", scrub="@appkind", keep="events 180 天。"))
row("usage", "stream_start / stream_end: sparxie_game", "`genshin`", "@game",
    "Which Sparxie games are used (catalogue ids only).", "No: a game of the Sparxie catalogue (nine ids) or `other`; a typed or unknown value becomes `other`.", "@game and an enum in the Worker @w_dims.", "events: 180 days.",
    dict(field="stream_start / stream_end：sparxie_game", purpose="哪些 Sparxie 游戏在使用（只用目录里的 id）。", risk="否：Sparxie 目录里的九个 id 之一，或 `other`；手写或未知的值变成 `other`。", scrub="@game，Worker 端枚举 @w_dims。", keep="events 180 天。"))
row("usage", "stream_end: duration_s, result", "`600`, `normal`", "@e_stream_end",
    "Total streaming time (the figure behind \"minutes streamed\"); how streams end.", "No: a number and one of four words.", "@e_stream_end", "events: 180 days; sessions keep the sum 400 days.",
    dict(field="stream_end：duration_s、result", purpose="串流总时长（「串流分钟数」的来源）；串流以什么方式结束。", risk="否：一个数字和四个词之一。", scrub="@e_stream_end", keep="events 180 天；sessions 保存合计 400 天。"))
row("usage", "feature_used: feature", "`stream.hdr`, `web.app_added`, `panel.pair_open`", "@e_feature @ingest",
    "Which features are used at all.", "No: our own ids. A page may name a feature only with an id of its own product (`web.` or `panel.`) @ingest.", "Lower-case token @e_feature; prefix check for pages @ingest.", "events: 180 days.",
    dict(field="feature_used：feature", purpose="哪些功能真的有人用。", risk="否：我们自己的 id。网页只能用自己产品前缀（`web.` 或 `panel.`）的 id @ingest。", scrub="小写标记 @e_feature；网页事件的前缀检查 @ingest。", keep="events 180 天。"))
row("usage", "setting_changed: key, value", "`max_bitrate`, `15000-30000`", "@e_setting @settings",
    "Which settings people change, and to what kind of value.", "No: a key from a list of 27 and a value that is a word from a list, or a range bucket. Free values such as paths, names and addresses are not in the list.", "@settings, and the Worker repeats the list and the value formats @w_schema.", "events: 180 days.",
    dict(field="setting_changed：key、value", purpose="人们改了哪些设置、改成什么类型的值。", risk="否：key 来自 27 项名单；value 是名单里的词或区间。路径、名称、地址这类自由值不在名单里。", scrub="@settings，Worker 重复检查名单和取值格式 @w_schema。", keep="events 180 天。"))

# --- experience
row("experience", "stream_quality (per stream)", "`fps_p50 119`, `encode_ms_p95 6`, `loss_pct 0.1`, `reconnects 0`", "@e_stream_quality",
    "Is streaming smooth? Frame rate, delay, loss, stalls by version and codec.", "No: numbers.", "Clamped numbers rounded to one decimal @e_stream_quality.", "events: 180 days; AE points per Cloudflare's retention.",
    dict(field="stream_quality（每次串流一条）", purpose="串流是否流畅？按版本和编码看帧率、延迟、丢包、卡顿。", risk="否：数字。", scrub="限幅并四舍五入到一位小数 @e_stream_quality。", keep="events 180 天；AE 数据点按 Cloudflare 的保留期。"))
row("experience", "stream_health (per stream-hour)", "`windows 60`, `bad {backlog_p95: 3}`, `queue_flushes 0`, `codec hevc`", "@e_stream_health",
    "Does a version stream worse than the one before it? Per minute: host processing, encode, capture, send time and send backlog; how many minutes were over a fixed bound; cleared queues, key-frame requests, loss. For a share of streams also a 19-bucket distribution per statistic.", "No: numbers and fixed words (codec, resolution, frame rate, encoder, capture method, graphics driver version).", "Allow-listed names, clamped numbers, fixed-word dimensions @e_stream_health.", "counters: 400 days; raw rows are not kept.",
    dict(field="stream_health（每串流小时一条）", purpose="新版本的串流是否比上一版更差？按分钟统计主机处理、编码、采集、发送耗时和发送积压，有多少分钟越过固定界限，队列被清空、关键帧请求、丢包；一部分串流还带每个指标的 19 桶分布。", risk="否：数字和固定词（编码、分辨率、帧率、编码器、采集方式、显卡驱动版本）。", scrub="只收白名单里的名字，数字限幅，维度是固定词 @e_stream_health。", keep="计数 400 天；不保留原始行。"))
row("experience", "stream_anomaly", "`code XP_QUEUE_FLUSH`, `value 31`, `count 992`, `log true`", "@e_stream_anomaly",
    "Which problem happened, how big, on which configuration; a serious one also sends the last two minutes of the host log (anonymised).", "Low: a fixed code and numbers; the log excerpt is cleaned text without a clock time, so what a log line says after cleaning is sent.", "Fixed codes only; the log excerpt drops text between quotes, brackets and braces (a short bracket that only holds a code, such as `[E12]`, stays), goes through the same cleaning as crash logs, and the computer and account names are removed @e_stream_anomaly.", "counters: 400 days; log excerpts: 90 days.",
    dict(field="stream_anomaly", purpose="发生了哪种问题、多严重、在什么配置上；严重时还会发送问题前两分钟的主机日志（已匿名）。", risk="低：固定代码和数字；日志片段是清洗后的文本，没有时钟时间，清洗后日志行里剩下的内容会被发送。", scrub="只收固定代码；日志片段丢弃引号、方括号和花括号里的文字（只装代码的短方括号如 `[E12]` 保留），用和崩溃日志相同的清洗规则，并去掉计算机名和账户名 @e_stream_anomaly。", keep="计数 400 天；日志片段 90 天。"))
row("experience", "stream_regress", "`metric jitter_p95`, `baseline 0.3`, `current 2.6`", "@e_stream_regress",
    "Did this installation get worse than it used to be? One statistic, its value before and now; at most once per version.", "No: a statistic name and two numbers. The rolling average is kept in the installation's own folder.", "Fixed statistic names, clamped numbers @e_stream_regress.", "counters: 400 days.",
    dict(field="stream_regress", purpose="这台安装是否比它自己以前更差？一个指标的名字、以前的值和现在的值；每个版本最多一次。", risk="否：指标名和两个数字；滚动平均保存在本机自己的目录里。", scrub="指标名固定，数字限幅 @e_stream_regress。", keep="计数 400 天。"))
row("experience", "startup: host_start_ms, first_frame_ms", "`2100`, `450`", "@e_startup",
    "How long until the host is ready and until the first frame.", "No: numbers.", "Clamped.", "events: 180 days.",
    dict(field="startup：host_start_ms、first_frame_ms", purpose="主机多久就绪、多久出第一帧。", risk="否：数字。", scrub="限幅。", keep="events 180 天。"))
row("experience", "panel_page: page, time_to_content_ms", "`apps`, `180`", "@e_panel_page",
    "How long a page of the panel takes to show its content.", "No: a route id of our own and a number.", "Token and range.", "events: 180 days.",
    dict(field="panel_page：page、time_to_content_ms", purpose="控制面板各页面多久显示出内容。", risk="否：我们自己的路由 id 和一个数字。", scrub="标记和范围。", keep="events 180 天。"))

row("experience", "page_vitals: page, visits, lcp, inp, cls, loaf, fpaint, tti, heap, loaf_n, loaf_block_ms, fr_a..fr_d, dwell_s", "`home`, `15`, `lcp [0,15,0,0,0,0,0,0]`, `fr_d 5`", "@e_page_vitals @js_perf",
    "Do the pages of the panel and the Web UI feel smooth? Per route and send: how many visits fell into each of eight fixed buckets of the largest content paint, the response to presses, layout shifts, the longest busy frame, the first paint, the time until the page says it is usable and the memory of the page; how many frames of a scroll or an animation took longer than 17, 34 and 50 ms.", "No: counts per bucket, a route word of our own and a few counters. No trace, no timestamp of a single event, no text.", "Eight counts per metric with bounds that are part of the contract @js_buckets, route words checked against the fixed pattern, counters clamped @e_page_vitals; the Worker adds the counts up per version and day @w_page.", "counters: 400 days; the per-route part in Analytics Engine.",
    dict(field="page_vitals：page、visits、lcp、inp、cls、loaf、fpaint、tti、heap、loaf_n、loaf_block_ms、fr_a..fr_d、dwell_s", purpose="面板和 Web 界面用起来是否流畅？按页面和每次发送：最大内容绘制、点击响应、版面跳动、最长的忙碌帧、首次绘制、页面自报可用的时间、页面内存各落在八个固定区间的哪一个，以及滚动或动画时超过 17、34、50 毫秒的帧数。", risk="否：每个区间的次数、我们自己的页面名和几个计数。没有单次事件的轨迹和时间戳，没有文字。", scrub="每个指标八个计数，区间边界是约定的一部分 @js_buckets；页面名按固定格式检查，计数限幅 @e_page_vitals；服务器按版本和日期把计数相加 @w_page。", keep="计数 400 天；按页面的部分在 Analytics Engine。"))
row("experience", "api_latency: endpoint, calls, errors, timeouts, lat", "`config`, `40`, `0`, `0`, `lat [0,40,0,0,0,0,0,0]`", "@e_api_latency @js_perf",
    "How quickly does the host answer the calls of the panel and the Web UI? Per endpoint word: calls, failed calls, timeouts and eight latency buckets.", "No: a word for the endpoint (the path without numbers and ids, at most two segments) and counts.", "Digits-only and long segments are dropped from the word, the telemetry calls are not timed, a call the page cancelled is not counted @js_perf @e_api_latency.", "counters: 400 days.",
    dict(field="api_latency：endpoint、calls、errors、timeouts、lat", purpose="主机回答面板和 Web 界面的请求有多快？按接口名：请求数、失败数、超时数和八个耗时区间。", risk="否：接口名（路径去掉数字和 id，至多两段）和计数。", scrub="只有数字的段和过长的段不进名字，上报本身不计时，页面自己取消的请求不计 @js_perf @e_api_latency。", keep="计数 400 天。"))
row("experience", "svc_lifecycle: launched, adopted, extras_ended, exits, unexpected, soon_after_adopt, restarts_core_exit, restarts_session", "`1`, `1`, `1`, `1`, `1`, `1`, `1`, `0`", "@e_svc_lifecycle @svcwrite",
    "What did the service do with the core? The service leaves a small file of facts (how a core was started or taken over, an extra one ended, a core that ended without being asked, how long it had run); the host reads it at its next start, reports the counts and deletes it.", "No: counts. The file has numbers and fixed words only, no path and no name.", "Counts clamped, the file is read line by line against word lists and ranges @e_svc_lifecycle @svcjudge.", "counters: 400 days.",
    dict(field="svc_lifecycle：launched、adopted、extras_ended、exits、unexpected、soon_after_adopt、restarts_core_exit、restarts_session", purpose="服务对核心做了什么？服务留下一个小文件（核心怎样被启动或接管、多余的核心被结束、没有被要求就结束的核心、运行了多久）；主机下次启动时读出、上报计数并删除。", risk="否：计数。文件里只有数字和固定词，没有路径和名称。", scrub="计数限幅，文件逐行按词表和范围检查 @e_svc_lifecycle @svcjudge。", keep="计数 400 天。"))
row("experience", "startup_timing: which, ms, slow", "`svc_core`, `34000`, `true`", "@e_startup_timing @svcjudge",
    "How long does it take until the interface can be used? From the start of the service to the web interface of the core (svc_core), and from the opening of a page to its ready mark (page); slow is above 15 s or well above this install's own usual time.", "No: a fixed word, a number of milliseconds and a yes or no. The install's usual times are kept in its own folder.", "Fixed words, clamped numbers @e_startup_timing.", "counters: 400 days.",
    dict(field="startup_timing：which、ms、slow", purpose="打开到可以使用要多久？从服务启动到核心的网页界面可用（svc_core），以及页面从打开到可用的标记（page）；慢指超过 15 秒或远高于这台电脑自己的常见耗时。", risk="否：固定词、毫秒数和是否。这台电脑自己的常见耗时保存在它自己的文件夹里。", scrub="固定词，数字限幅 @e_startup_timing。", keep="计数 400 天。"))
row("experience", "host_anomaly: code, severe, value, threshold, count, cause, which, exit_code", "`XP_SVC_CORE_RESTART`, `true`, `30000`, `120000`, `1`, `soon_after_adopt`", "@e_host_anomaly @svcjudge",
    "Did the core end without being asked and start again (XP_SVC_CORE_RESTART), especially soon after the service took over a running core? Was the start slow (XP_SLOW_READY)? It repeats or follows an adoption: priority 0.", "No: one of two fixed codes, numbers and fixed words; the exit code is a number.", "Fixed codes and causes, clamped numbers @e_host_anomaly.", "counters: 400 days.",
    dict(field="host_anomaly：code、severe、value、threshold、count、cause、which、exit_code", purpose="核心有没有在没被要求时结束并重新启动（XP_SVC_CORE_RESTART），尤其是服务接管一个已在运行的核心后不久？启动是否很慢（XP_SLOW_READY）？重复出现或发生在接管之后：优先级 0。", risk="否：两个固定代码之一、数字和固定词；退出码是一个数字。", scrub="固定代码和原因，数字限幅 @e_host_anomaly。", keep="计数 400 天。"))
row("experience", "device_facts: gpu_vendor, gpu_driver, os_build, displays, refresh_hz, nic_link, nic_speed", "`nvidia`, `32.0.15.6094`, `26100`, `2`, `144`, `wired`, `2_5g`", "@e_device_facts @facts",
    "Which kind of computer has a problem? Coarse facts, sent only when they change (at most once a day): graphics vendor and driver version, the Windows build, the number of screens, the highest refresh rate, and whether the network adapter in use is wired or wireless and its speed class.", "No: vendor words, version numbers, counts and classes. No device name, no serial number, no address.", "Only the vendor word and the digits of the driver and the build are kept; the refresh rate is snapped to a standard rate and the adapter speed to a class @facts @e_device_facts.", "counters: 400 days.",
    dict(field="device_facts：gpu_vendor、gpu_driver、os_build、displays、refresh_hz、nic_link、nic_speed", purpose="哪类电脑出了问题？粗略的配置，只在变化时发送（每天至多一次）：显卡厂商和驱动版本、Windows 版本号、屏幕数量、最高刷新率、正在使用的网卡是有线还是无线及其速度档。", risk="否：厂商词、版本号、数量和档位。没有设备名、序列号和地址。", scrub="只保留厂商词、驱动和系统版本号里的数字；刷新率取最接近的标准值，网卡速度只取档位 @facts @e_device_facts。", keep="计数 400 天。"))

# --- errors
row("error", "error: component, code, severity, count", "`stream`, `e1a2b3c4`, `error`, `3`", "@e_error @logs",
    "Which component fails and how often. The code is a hash of the fixed start of the log line (up to the first quote, bracket, colon or digit), so the same error is one code.", "No: no message. Before this fix an error carried a scrubbed log line of up to 200 characters, which could still hold a device or game name; now the line stays on the computer.", "The message property is gone from the allow-list in the sender @e_error, from the pages @js_err @js_issue and from the Worker @w_schema; an old client that still sends one has it dropped.", "events: 180 days.",
    dict(field="error：component、code、severity、count", purpose="哪个组件出错、出错多频繁。code 是日志行固定开头（到第一个引号、括号、冒号或数字为止）的哈希，所以同一个错误是同一个 code。", risk="否：不含文字。修复前，错误事件带一行最长 200 字符、经过清洗的日志，仍可能含设备名或游戏名；现在这行日志留在本机。", scrub="发送端白名单 @e_error、网页端 @js_err @js_issue、Worker @w_schema 都去掉了 message 属性；仍发送它的旧客户端，该属性会被丢弃。", keep="events 180 天。"))
row("error", "update_result: source, stage, result, error_class, from_version, to_version, dur_ms, reboot_needed", "`mirror`, `download`, `failed`, `http_5xx`", "@e_update_result",
    "Which step of an update fails, from which source and for what kind of reason? One event per finished step.", "No: fixed words (source, step, result, class of the reason), version numbers and a duration. The text of an error is never sent.", "Fixed words, version numbers reduced to digits and dots @e_update_result.", "counters: 400 days.",
    dict(field="update_result：source、stage、result、error_class、from_version、to_version、dur_ms、reboot_needed", purpose="更新的哪一步失败、来自哪个来源、是什么类别的原因？每个结束的步骤一条。", risk="否：固定词（来源、步骤、结果、原因类别）、版本号和耗时。从不发送错误文字。", scrub="固定词，版本号只留数字和点 @e_update_result。", keep="计数 400 天。"))
row("error", "install_result: result, error_class, phase, component, step, exit_code, reboot_needed, from_version, dur_ms", "`failed`, `other`, `upgrade`, `usbip`, `roothub_guard`, `102`", "@e_install_result @journal",
    "Which component of an installation or upgrade fails, at which step and with which exit code, and from which version? The installer leaves a journal; the host reads it at its next start, reports it and deletes it.", "No: component and step words from a fixed list, the exit code, a version number and a duration. Paths and the text of an error are not in the journal.", "The journal is checked line by line against word lists and ranges, an unknown component is dropped @journal @e_install_result.", "counters: 400 days.",
    dict(field="install_result：result、error_class、phase、component、step、exit_code、reboot_needed、from_version、dur_ms", purpose="安装或升级的哪个组件在哪一步失败、退出码是什么、从哪个版本升级？安装器留下一份记录，主机下次启动时读取、上报并删除。", risk="否：固定列表里的组件词和步骤词、退出码、版本号和耗时。记录里没有路径和错误文字。", scrub="逐行按词表和范围检查，不认识的组件被丢弃 @journal @e_install_result。", keep="计数 400 天。"))
row("error", "install_summary: phase, result, components, failed, reboot_needed, from_version, dur_ms", "`upgrade`, `failed`, `5`, `1`", "@e_install_summary @journal",
    "Did an installation finish? One event per run: ok, failed, cancelled, or incomplete (it started and never finished, the journal was quiet for ten minutes).", "No: fixed words and counts.", "Fixed words and clamped numbers @e_install_summary.", "counters: 400 days.",
    dict(field="install_summary：phase、result、components、failed、reboot_needed、from_version、dur_ms", purpose="安装有没有完成？每次运行一条：成功、失败、取消，或「中断」（开始后一直没有结束，记录已安静十分钟）。", risk="否：固定词和计数。", scrub="固定词，数字限幅 @e_install_summary。", keep="计数 400 天。"))
row("error", "error_report: component, lang, fingerprint, code, severity, loc, frames, crumbs, log, build, page, count", "`host`, `cpp`, `a1b2c3d4e5f60789`, `E_ENCODER_INIT`, `src/video.cpp:4821:encoder_init`", "@e_error_report",
    "Where in the program an error happens and what the person did just before. The first report of an error leaves at once, later ones are counted and leave as one count every ten minutes; at most 20 reports an hour.", "Low: the location and the stack are fixed shapes (module+0xoffset, src/...:line:function of our own source tree, function@file.js:line:col of a page); an absolute path, a drive letter or a home folder is dropped whole. Breadcrumbs are our own ids of the last 50 actions. log is the last up to 40 lines (200 characters each) of the component's own log: it stays text.", "The component anonymises its log first; the host cleans every line again (names, folders, addresses including IPv6, e-mail addresses, keys; text between quotes, brackets and braces dropped). Sent only while error reports are on, with the first report of an error. Actions are not recorded while error reports are off, and what was held back when the choice turned off is dropped @e_error_report.", "counters 400 days; the first sample of a group 400 days.",
    dict(field="error_report：component、lang、fingerprint、code、severity、loc、frames、crumbs、log、build、page、count", purpose="错误发生在程序的哪个位置，以及出错前用户做了什么。同一个错误第一次立即发送，之后的只计数，每十分钟合并成一条数量发送；每小时最多 20 条。", risk="低：位置和栈只有固定的几种形状（模块+0x偏移、我们自己源码树里的 src/…:行:函数、页面的 函数@文件名:行:列）；绝对路径、盘符、用户文件夹整条丢弃。面包屑是我们自己的 id，最近 50 个动作。log 是组件自己日志的最后至多 40 行（每行至多 200 个字符），仍然是文本。", scrub="组件先把日志匿名处理，主机再清洗每一行（名称、文件夹、包含 IPv6 在内的地址、邮箱、密钥；引号、方括号和花括号里的文字被丢弃）。仅在错误报告开启时、随一个错误的第一条报告发送。错误报告关闭期间不记录动作，关闭时已保留的内容被丢弃 @e_error_report。", keep="计数 400 天；分组的第一份样本 400 天。"))
row("error", "wer_event: event_id, kind, module, exception_code, offset", "`1000`, `crash`, `zakovdd.dll`, `c0000005`, `0x1a2b3c`", "@e_wer_event",
    "Did the virtual display driver fault? Windows writes it to the Application log; the host reads it at start.", "No: only a fault whose module is the driver's own file is taken, with the exception code and the offset; no stack, and the event data of every other program is never copied.", "Fixed shape and module name; sent only while error reports are on @e_wer_event.", "events: 180 days.",
    dict(field="wer_event：event_id、kind、module、exception_code、offset", purpose="虚拟显示器驱动有没有出过故障？Windows 把它写在应用程序日志里，主机启动时读取。", risk="否：只取故障模块是驱动自己文件的那一条，带异常代码和偏移；没有栈，其他程序的事件数据从不复制。", scrub="固定形状和模块名；仅在错误报告开启时发送 @e_wer_event。", keep="events 180 天。"))
row("error", "log_summary: component, fatal, error, warning", "`encoder`, `0`, `2`, `9`", "@e_log_summary @logs",
    "How many warnings and errors the host logs per hour, by component.", "No: counts. The component is one of 24 fixed words or `host`.", "@logs", "events: 180 days.",
    dict(field="log_summary：component、fatal、error、warning", purpose="主机每小时按组件记录了多少警告和错误。", risk="否：计数。组件是 24 个固定词之一，或 `host`。", scrub="@logs", keep="events 180 天。"))
row("error", "user_report: category, component, page, error_code, attachment, message", "`bug`, `web`, `apps`, `E_LOST`, `att-2026-11/<sha256>.webp`, `the apps page stays empty`", "@e_user_report @js_report @js_msg",
    "A report the person writes and presses Send on. Answers \"what was broken for this person\".", "Yes, by nature: it is text a person typed. Besides it, the other texts are the logs listed under Never sent: the program log that goes with it (`log_ref`), the log excerpt after a serious stream problem and the log lines of an error report. It is sent only when they press Send, after the form has shown exactly what leaves (title, description, a short summary of the error with its top stack lines and last events).", "Scrubbed in the browser @js_scrub and again in the host @scrub and the Worker @w_scrub; at most 2000 characters; no contact field exists; the attachment is only a reference the developer panel returned.", "events: 180 days; the GitHub issue until the developers close it.",
    dict(field="user_report：category、component、page、error_code、attachment、message", purpose="用户亲手写、亲手点击「发送」的报告，回答「这个人遇到了什么问题」。", risk="是，这是它的性质：它是人打的字，除它之外，其他文本是「从不发送」里列出的几种日志：随它附带的程序日志（`log_ref`）、严重串流问题之后的日志摘录和错误报告里的日志行。只有按下「发送」才会发出，且表单会先展示将发送的全部内容（标题、描述、错误的简短摘要及堆栈开头几行、最近的事件）。", scrub="在浏览器 @js_scrub、主机 @scrub 和 Worker @w_scrub 各清洗一遍；最长 2000 字符；没有联系方式字段；附件只是开发者面板返回的引用。", keep="events 180 天；GitHub 问题在开发者关闭前保留。"))

# --- crash
row("crash", "meta: product, version, os_version, time, uptime_s", "`host`, `2026.1112.0.杂鱼`, `10.0.26100`", "@meta",
    "Which release crashes, and how long after start.", "No.", "Same checks as the envelope.", "crash_reports: 400 days.",
    dict(field="meta：product、version、os_version、time、uptime_s", purpose="哪个版本崩溃、启动多久后崩溃。", risk="否。", scrub="与信封相同的检查。", keep="crash_reports 400 天。"))
row("crash", "meta: exception_code, module, module_offset, frames[≤64]", "`0xC0000005`, `sunshine.exe`, `0x1a2b3c`", "@meta @basename @w_crash",
    "Where in the program the crash happens, so it can be found with the unstripped binary of the release.", "No: a file name and an address inside our own binary. Directories are cut off in the sender @basename and again in the Worker @w_crash (a path would hold the account name).", "File name only; hex offsets; the code is a fixed shape.", "crash_signatures: 400 days.",
    dict(field="meta：exception_code、module、module_offset、frames（至多 64 个）", purpose="崩溃发生在程序的哪个位置，以便对照该版本未剥离符号的二进制找到它。", risk="否：文件名和我们自己二进制里的地址。目录在发送端 @basename 和 Worker @w_crash 都会被去掉（路径里会有账号名）。", scrub="只留文件名；十六进制偏移；代码是固定形状。", keep="crash_signatures 400 天。"))
row("crash", "signature", "`ab…ab` (64 hex)", "@meta",
    "Group identical crashes.", "No: SHA-256 of the first eight frames' module names and offsets.", "—", "crash_signatures: 400 days.",
    dict(field="signature", purpose="把相同的崩溃归为一组。", risk="否：前八帧的模块名和偏移的 SHA-256。", scrub="—", keep="crash_signatures 400 天。"))
row("crash", "dump (host, Panel, sparxie-launch)", "a file of tens to a few hundred KB", "@dumptype @dumpfilter @redact @privnames @rustdump @rustredact @csdump @csredact",
    "Lets the developers read the stack of a crash they cannot reproduce.", "Reduced to the least possible. Before this fix the host and the Panel asked for \"indirectly referenced memory\" (the heap behind every stack pointer: TLS keys, pairing secrets, web UI sessions, clipboard text can live there). Now: no heap, no data segments, no handle data, only the pointer-sized values of each stack (MiniDumpFilterMemory), module paths filtered (MiniDumpFilterModulePaths), and a callback that keeps only module names and CodeView ids, thread records, contexts and stacks. On the computer, before upload, the account name behind `\\Users\\`, `/Users/`, `/home/` and the user and computer names are overwritten in place (ASCII and UTF-16) @redact. Possible residue: a pointer-sized value on a stack that happens to look like text; mitigated by the filter, not eliminated. It is why the dump is only sent with error reports on.", "Dump type and callback @dumptype; byte redaction in three implementations with the same test vectors.", "Release asset of a private GitHub repository, deleted after 90 days @w_dumpret; at most 20 per signature, one per install and signature @w_dumps.",
    dict(field="dump（主机、控制面板、sparxie-launch）", purpose="让开发者能读到自己复现不了的崩溃的调用栈。", risk="已压到最小。修复前，主机和控制面板请求了「间接引用内存」（每个栈指针指向的堆：TLS 私钥、配对密钥、Web UI 会话、剪贴板文字都可能在里面）。现在：没有堆、没有数据段、没有句柄数据，每个栈只保留指针大小的数值（MiniDumpFilterMemory），模块路径被过滤（MiniDumpFilterModulePaths），回调只保留模块名和 CodeView id、线程记录、上下文和栈。上传前，在本机把 `\\Users\\`、`/Users/`、`/home/` 后的账号名以及用户名、电脑名就地覆盖（ASCII 和 UTF-16）@redact。", scrub="转储类型和回调 @dumptype；字节级清洗有三种实现，使用同样的测试向量。", keep="私有 GitHub 仓库的 release 资产，90 天后删除 @w_dumpret；每个签名最多 20 份，每台安装每个签名一份 @w_dumps。"))

# --- storage
STORE = [
    ("D1 activity", "day, product, install_hash, version, channel, os, country, update_source", "@w_activity", "Count users per day.", "400 days @w_retention"),
    ("D1 sessions", "install_hash, session_id, day, product, version, channel, os, os_version, arch, gpu_vendor, locale, country, first_ms, last_ms, events, streams, stream_s, errors, exited, codec", "@w_sessions", "One summary per run: length, streams, errors, crash-free rate.", "400 days"),
    ("D1 events", "day, t, install_hash, session_id, product, version, kind, name, props (JSON of the allow-listed props)", "@w_events", "Percentiles, top errors, panel time to content.", "180 days"),
    ("D1 crash_signatures", "signature, version, product, kind, exception_code, module, module_offset, frames, first_ms, last_ms, count", "@w_sigs", "Group crashes, mark new ones.", "400 days"),
    ("D1 crash_reports", "day, time_ms, install_hash, signature, version, product, kind, os_version, uptime_s, country", "@w_reports", "Crash rate per release.", "400 days"),
    ("D1 crash_dumps", "signature, install_hash, tag, name, bytes, stored_ms (the dump itself is a release asset)", "@w_dumps", "Index of the stored dumps.", "90 days with the dump"),
    ("D1 alerts, counters, report_state", "alert key, title, body, labels, issue number; rate-limit counters; day of the daily report", "@w_alerts @w_counters", "De-duplicate alerts and reports; stay inside the free tier.", "7 to 400 days"),
    ("D1 page_hist, page_total, page_installs, api_hist, api_total", "day, product, version, metric or endpoint, bucket, n (counts per fixed bucket); totals of visits and frames; the install hashes that sent a page summary on a day", "@w_page", "Page smoothness and the speed of the host's answers per version.", "400 days (the install hashes 35 days)"),
    ("D1 install_daily, install_installs, install_runs, update_daily, device_dim", "day, product, version, phase, component, step, result, class, exit code, upgrade-from version, n; the install hashes that met a failure (35 days); runs by outcome; update steps by source and stage; counts per device fact value", "@w_install", "See install and update failures per version, component and exit code; which computers have problems.", "400 days (the install hashes 35 days)"),
    ("D1 svc_total, ready_hist, hostanom_daily, hostanom_installs", "day, product, version, counts of what the service did with the core, time-to-ready buckets by kind, anomaly counts by code, the install hashes that met an anomaly (35 days)", "@w_svc", "See per version whether the core ends by itself, whether starts got slower, and which installs met XP_SVC_CORE_RESTART or XP_SLOW_READY.", "400 days (the install hashes 35 days)"),
    ("Analytics Engine", "index1 = product; blobs: kind, name, version, channel, os, country, gpu_vendor, arch, install_hash and the string props; doubles: the numeric props", "@w_ae", "Fast aggregates of high-volume points.", "Cloudflare's retention"),
    ("GitHub issues (private repo)", "daily report (totals and top lists), crash alerts (signature, product, version, exception, module, frames), user reports (category, product, version, channel, os, os_version, country, where, install hash prefix of 8 characters, time, message in a code fence)", "@w_report @w_alertsrc @w_userrep", "The maintainers read reports and alerts.", "until closed"),
]
STORE_ZH = [
    "每天的用户数。", "每次运行一条摘要：时长、串流数、错误数、无崩溃比例。", "分位数、错误排行、页面出内容耗时。", "崩溃分组、标出新的崩溃。", "各版本崩溃率。", "已保存转储的索引（转储本身是 release 资产）。", "提醒和报告去重；不超出免费额度。", "按版本看页面是否流畅、主机回答是否变慢。", "按版本、组件、步骤和退出码看安装与更新的失败；哪类电脑有问题。", "按版本看核心是否自己结束、启动是否变慢，哪些安装遇到了异常。", "高频数据点的快速汇总。", "负责人阅读报告和提醒。",
]
STORE_KEEP_ZH = ["400 天", "400 天", "180 天", "400 天", "400 天", "与转储一起 90 天", "7 到 400 天", "400 天（安装哈希 35 天）", "400 天（安装哈希 35 天）", "400 天（安装哈希 35 天）", "Cloudflare 的保留期", "直到关闭"]

NEVER_EN = [
    "Screen content, audio, keystrokes, files, clipboard content.",
    "The name of the computer, the Windows or macOS account, a device or a client; the name of a game or an application (a game is at most one of nine catalogue words).",
    "File paths, IP addresses, MAC addresses, ports, pairing identifiers, Steam, miHoYo or other account ids, Mirror酱 CDKs, passwords, tokens, cookies.",
        "Free text has exactly four places, and only while error reports are on (the first needs the person to press Send): (1) `user_report.message`, sent when the person presses Send, after the form showed it; (2) the program log attached to a crash report or a feedback report (`user_report.log_ref`), anonymised on this computer with the rules of the \"program log\" row above (user and computer names, user segments of paths, IP and MAC addresses, e-mail, serial numbers, tokens, PINs, Wi-Fi names, game account names; a name that stands between quotes is not removed by those rules unless it is one of these things); (3) the log excerpt after a serious stream problem (`stream_anomaly`, `log true`): the last two minutes of the host's own log; (4) `error_report.log`: the last up to 40 lines (200 characters each) of a component's own log, with the first report of an error. For (3) and (4) text between quotes, brackets and braces is dropped as well (a short bracket that only holds a code, such as `[E12]`, stays) and names, folders, addresses (IPv4 and IPv6), e-mail addresses and keys are cleaned. All four stay text, not a fixed list of words, so what a line happens to say after the cleaning is sent. Everything else is a fixed word, a number or an id of our own.",
]
NEVER_ZH = [
    "屏幕内容、声音、键盘输入、文件、剪贴板内容。",
    "电脑名、Windows 或 macOS 账号名、设备名或客户端名；游戏名和应用名（游戏最多是九个目录词之一）。",
    "文件路径、IP 地址、MAC 地址、端口、配对标识、Steam、米哈游或其他账号 id、Mirror酱 CDK、密码、令牌、Cookie。",
    "自由文本只有四处，除第一处外都仅在错误报告开启时发送（第一处需要用户按下「发送」）：（1）`user_report.message`：用户按下「发送」后发出，表单事先展示过；（2）随崩溃报告或反馈报告附带的程序日志（`user_report.log_ref`），在本机按上面「程序日志」一行的规则匿名处理（用户名和电脑名、路径里的用户名、IP 和 MAC 地址、邮箱、序列号、令牌、PIN、Wi-Fi 名、游戏账号名；引号里的名字除非属于这些类别，否则不会被这些规则去掉）；（3）严重串流问题之后的日志摘录（`stream_anomaly`，`log true`）：主机日志最近两分钟；（4）`error_report.log`：组件自己日志的最后至多 40 行（每行至多 200 个字符），随一个错误的第一条报告发送。（3）和（4）还会丢弃引号、方括号和花括号里的文字（只装代码的短方括号如 `[E12]` 保留），并清洗名称、文件夹、地址（IPv4 和 IPv6）、邮箱和密钥。四处都仍然是文本，不是固定词表，所以清洗后一行恰好写了什么就会被发送。其余都是固定词、数字或我们自己的 id。",
]

def render(lang):
    zh = lang == "zh"
    out = []
    w = lambda text: out.append(fill(text))
    if zh:
        w("# 隐私数据清单：每个客户端发送什么、服务器保存什么\n")
        w("这份清单回答三个问题：发送了什么、为什么需要、怎样去除了个人信息。每一条都标出了代码位置（`文件:行号`）；代码位置由 `scripts/telemetry/build_inventory.py` 在生成时查找，所以会随代码更新。\n")
        w("## 一句话\n")
        w("两个独立的开关（错误报告、使用统计），默认开启，关闭后该类内容不再发送。数据只发往开发者自己的服务器 `api.alkaidlab.com`（Cloudflare Worker），不经过广告网络，不出售。除用户亲手写的反馈、随崩溃或反馈报告附带的匿名程序日志、严重串流问题之后的日志摘录和错误报告携带的组件日志最后几行（仅在错误报告开启时，清洗后的文本，见「从不发送」）外，没有任何自由文本；名称、路径、地址、账号在发送前就不在载荷里。")
        w("\n服务器自己不保存 IP 地址，只保存 Cloudflare 给出的国家代码；Workers 日志已关闭 @w_obs。连接本身必然经过网络，Cloudflare 作为网络服务商在传输层看得到连接的 IP，这一层不由我们控制，我们的代码不读取、不保存、不记录它。\n")
        heads = ["字段", "示例", "代码位置", "用途（回答什么问题）", "能否识别个人或泄露私密内容", "已做的清洗", "保留时间"]
        groups = [("envelope", "每个批次都带的信封"), ("usage", "使用事件（使用统计）"), ("experience", "体验事件（使用统计）"), ("error", "错误事件（错误报告）"), ("crash", "崩溃报告（错误报告）")]
    else:
        w("# Privacy data inventory: what each client sends and what the server keeps\n")
        w("This list answers three questions: what is sent, why it is needed and how personal details are removed. Every entry names the code (`file:line`); the code locations are found by `scripts/telemetry/build_inventory.py` when the page is generated, so they follow the code.\n")
        w("## In one paragraph\n")
        w("Two independent switches (error reports, usage statistics), on by default; when one is off, nothing of that kind is sent. The data goes only to the developers' own server, `api.alkaidlab.com` (a Cloudflare Worker), not through an advertising network, and is not sold. Apart from feedback a person writes by hand, the anonymised program log that goes with a crash report or a feedback report, the log excerpt after a serious stream problem and the last lines of a component's own log that an error report carries (only while error reports are on, cleaned text, see Never sent) there is no free text anywhere: names, paths, addresses and accounts are not in the payload to begin with, and are removed from the logs before they leave the computer.")
        w("\nThe server does not store IP addresses, only the country code Cloudflare supplies; Workers Logs are off @w_obs. The connection itself necessarily crosses the network and Cloudflare, as the network provider, sees the connecting address at the transport layer; that layer is not ours, and our code does not read, store or log it.\n")
        heads = ["Field", "Example", "Code", "Purpose (which question it answers)", "Could it identify a person or reveal private content?", "Scrubbing applied", "Retention"]
        groups = [("envelope", "Sent with every batch (the envelope)"), ("usage", "Usage events (usage statistics)"), ("experience", "Experience events (usage statistics)"), ("error", "Error events (error reports)"), ("crash", "Crash reports (error reports)")]
    for gid, title in groups:
        w(f"## {title}\n")
        w("| " + " | ".join(heads) + " |")
        w("|" + " --- |" * len(heads))
        for r in R:
            if r["group"] != gid:
                continue
            z = r["zh"]
            cells = [z["field"], r["example"], r["src"], z["purpose"], z["risk"], z["scrub"], z["keep"]] if zh else [r["field"], r["example"], r["src"], r["purpose"], r["risk"], r["scrub"], r["keep"]]
            cells = [fill(c).replace("|", "\\|").replace("\n", " ") for c in cells]
            w("| " + " | ".join(cells) + " |")
        w("")
    w("## " + ("服务器保存的数据" if zh else "What the Worker stores") + "\n")
    if zh:
        w("| 存放处 | 列 / 内容 | 代码位置 | 用途 | 保留时间 |")
    else:
        w("| Store | Columns / content | Code | Purpose | Retention |")
    w("| --- | --- | --- | --- | --- |")
    for i, (name, cols, src, purpose, keep) in enumerate(STORE):
        cells = [name, cols, src, STORE_ZH[i] if zh else purpose, STORE_KEEP_ZH[i] if zh else keep]
        w("| " + " | ".join(fill(c).replace("|", "\\|") for c in cells) + " |")
    w("")
    w("## " + ("从不发送" if zh else "Never sent") + "\n")
    for line in (NEVER_ZH if zh else NEVER_EN):
        w("- " + line)
    w("")
    w("## " + ("怎样保证" if zh else "How it is enforced") + "\n")
    if zh:
        w("- 发送端有白名单：未列出的事件名和属性在离开电脑前就被丢弃 @e_error（单元测试 `tests/unit/test_telemetry.cpp`）。")
        w("- Worker 对同一份名单再检查一次；格式不对的属性被丢弃，不会被「清洗后保存」@w_schema。")
        w("- 三种语言的清洗规则用同一份测试向量：`tests/data/telemetry_scrub_vectors.json`（C++），m3e `test/scrub-vectors.json`（JS），Worker `test/scrub-vectors.json`（TS）；`scripts/telemetry/check-scrub-vectors.sh` 检查三份相同。向量里是虚构的用户名、电脑名、路径、IP、MAC、UUID、邮箱、令牌。")
        w("- 崩溃转储的字节级清洗在 C++、Rust、C# 各有一份实现和同样的测试向量。")
        w("- 同意：错误报告和使用统计各自独立；主机在每一次发送前都检查，网页发来的事件由主机再检查一次 @ingest。")
        w("- 用户可随时在「关于 › 隐私」看到「会发送什么」，并用「查看将发送的内容」看到下一次发送的原文 @preview。")
    else:
        w("- The sender has an allow-list: an event name or a property that is not listed is dropped before it leaves the computer @e_error (unit tests in `tests/unit/test_telemetry.cpp`).")
        w("- The Worker checks the same list again; a property of the wrong shape is dropped, never \"cleaned and kept\" @w_schema.")
        w("- The three scrubbers run the same test vectors: `tests/data/telemetry_scrub_vectors.json` (C++), m3e `test/scrub-vectors.json` (JS), Worker `test/scrub-vectors.json` (TS); `scripts/telemetry/check-scrub-vectors.sh` checks the three copies are identical. The vectors plant a fictional account name, computer name, paths, IPs, MACs, UUIDs, e-mail and tokens.")
        w("- The byte-level redaction of crash dumps has one implementation each in C++, Rust and C#, with the same test vectors.")
        w("- Consent: error reports and usage statistics are independent; the host checks before every send, and events from the pages are checked once more by the host @ingest.")
        w("- The person can read \"what is sent and what is not\" under About › Privacy and open \"See what will be sent\" for the exact next send @preview.")
    w("")
    return "\n".join(out)

if __name__ == "__main__":
    for lang, path in (("en", HOST + "docs/privacy-data-inventory.md"), ("zh", HOST + "docs/privacy-data-inventory.zh.md")):
        open(path, "w", encoding="utf-8").write(render(lang))
        print("wrote", path)
