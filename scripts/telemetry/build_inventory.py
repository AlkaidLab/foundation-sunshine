#!/usr/bin/env python3
"""Builds docs/privacy-data-inventory.md and docs/privacy-data-inventory.zh.md in the host repo from one table.
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
    "e_startup": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "startup", \{'),
    "e_panel_page": (HOST, "src/telemetry/events.cpp", r'kind_e::experience, "panel_page", \{'),
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
row("usage", "app_start: cold, start_ms, update_source", "`true`, `1800`, `mirror`", "@e_app_start",
    "How long start takes; where updates come from (Mirror酱 or GitHub).", "No: a boolean, a number, one of three words.", "Allow-list of types and ranges @e_app_start.", "events: 180 days.",
    dict(field="app_start：cold、start_ms、update_source", purpose="启动耗时；更新来源（Mirror酱还是 GitHub）。", risk="否：一个布尔值、一个数字、三个词之一。", scrub="按类型和范围的白名单 @e_app_start。", keep="events 180 天。"))
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
row("experience", "startup: host_start_ms, first_frame_ms", "`2100`, `450`", "@e_startup",
    "How long until the host is ready and until the first frame.", "No: numbers.", "Clamped.", "events: 180 days.",
    dict(field="startup：host_start_ms、first_frame_ms", purpose="主机多久就绪、多久出第一帧。", risk="否：数字。", scrub="限幅。", keep="events 180 天。"))
row("experience", "panel_page: page, time_to_content_ms", "`apps`, `180`", "@e_panel_page",
    "How long a page of the panel takes to show its content.", "No: a route id of our own and a number.", "Token and range.", "events: 180 days.",
    dict(field="panel_page：page、time_to_content_ms", purpose="控制面板各页面多久显示出内容。", risk="否：我们自己的路由 id 和一个数字。", scrub="标记和范围。", keep="events 180 天。"))

# --- errors
row("error", "error: component, code, severity, count", "`stream`, `e1a2b3c4`, `error`, `3`", "@e_error @logs",
    "Which component fails and how often. The code is a hash of the fixed start of the log line (up to the first quote, bracket, colon or digit), so the same error is one code.", "No: no message. Before this fix an error carried a scrubbed log line of up to 200 characters, which could still hold a device or game name; now the line stays on the computer.", "The message property is gone from the allow-list in the sender @e_error, from the pages @js_err @js_issue and from the Worker @w_schema; an old client that still sends one has it dropped.", "events: 180 days.",
    dict(field="error：component、code、severity、count", purpose="哪个组件出错、出错多频繁。code 是日志行固定开头（到第一个引号、括号、冒号或数字为止）的哈希，所以同一个错误是同一个 code。", risk="否：不含文字。修复前，错误事件带一行最长 200 字符、经过清洗的日志，仍可能含设备名或游戏名；现在这行日志留在本机。", scrub="发送端白名单 @e_error、网页端 @js_err @js_issue、Worker @w_schema 都去掉了 message 属性；仍发送它的旧客户端，该属性会被丢弃。", keep="events 180 天。"))
row("error", "log_summary: component, fatal, error, warning", "`encoder`, `0`, `2`, `9`", "@e_log_summary @logs",
    "How many warnings and errors the host logs per hour, by component.", "No: counts. The component is one of 24 fixed words or `host`.", "@logs", "events: 180 days.",
    dict(field="log_summary：component、fatal、error、warning", purpose="主机每小时按组件记录了多少警告和错误。", risk="否：计数。组件是 24 个固定词之一，或 `host`。", scrub="@logs", keep="events 180 天。"))
row("error", "user_report: category, component, page, error_code, attachment, message", "`bug`, `web`, `apps`, `E_LOST`, `att-2026-11/<sha256>.webp`, `the apps page stays empty`", "@e_user_report @js_report @js_msg",
    "A report the person writes and presses Send on. Answers \"what was broken for this person\".", "Yes, by nature: it is text a person typed. It is the only free text in the system. It is sent only when they press Send, after the form has shown exactly what leaves (title, description, a short summary of the error with its top stack lines and last events).", "Scrubbed in the browser @js_scrub and again in the host @scrub and the Worker @w_scrub; at most 2000 characters; no contact field exists; the attachment is only a reference the developer panel returned.", "events: 180 days; the GitHub issue until the developers close it.",
    dict(field="user_report：category、component、page、error_code、attachment、message", purpose="用户亲手写、亲手点击「发送」的报告，回答「这个人遇到了什么问题」。", risk="是，这是它的性质：它是人打的字，也是整个系统里唯一的自由文本。只有按下「发送」才会发出，且表单会先展示将发送的全部内容（标题、描述、错误的简短摘要及堆栈开头几行、最近的事件）。", scrub="在浏览器 @js_scrub、主机 @scrub 和 Worker @w_scrub 各清洗一遍；最长 2000 字符；没有联系方式字段；附件只是开发者面板返回的引用。", keep="events 180 天；GitHub 问题在开发者关闭前保留。"))

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
    ("Analytics Engine", "index1 = product; blobs: kind, name, version, channel, os, country, gpu_vendor, arch, install_hash and the string props; doubles: the numeric props", "@w_ae", "Fast aggregates of high-volume points.", "Cloudflare's retention"),
    ("GitHub issues (private repo)", "daily report (totals and top lists), crash alerts (signature, product, version, exception, module, frames), user reports (category, product, version, channel, os, os_version, country, where, install hash prefix of 8 characters, time, message in a code fence)", "@w_report @w_alertsrc @w_userrep", "The maintainers read reports and alerts.", "until closed"),
]
STORE_ZH = [
    "每天的用户数。", "每次运行一条摘要：时长、串流数、错误数、无崩溃比例。", "分位数、错误排行、页面出内容耗时。", "崩溃分组、标出新的崩溃。", "各版本崩溃率。", "已保存转储的索引（转储本身是 release 资产）。", "提醒和报告去重；不超出免费额度。", "高频数据点的快速汇总。", "负责人阅读报告和提醒。",
]
STORE_KEEP_ZH = ["400 天", "400 天", "180 天", "400 天", "400 天", "与转储一起 90 天", "7 到 400 天", "Cloudflare 的保留期", "直到关闭"]

NEVER_EN = [
    "Screen content, audio, keystrokes, files, clipboard content.",
    "The name of the computer, the Windows or macOS account, a device or a client; the name of a game or an application (a game is at most one of nine catalogue words).",
    "File paths, IP addresses, MAC addresses, ports, pairing identifiers, Steam, miHoYo or other account ids, Mirror酱 CDKs, passwords, tokens, cookies.",
    "Any free text the person did not write for a report: the only free text in the system is `user_report.message`, sent when they press Send, after the form showed it.",
]
NEVER_ZH = [
    "屏幕内容、声音、键盘输入、文件、剪贴板内容。",
    "电脑名、Windows 或 macOS 账号名、设备名或客户端名；游戏名和应用名（游戏最多是九个目录词之一）。",
    "文件路径、IP 地址、MAC 地址、端口、配对标识、Steam、米哈游或其他账号 id、Mirror酱 CDK、密码、令牌、Cookie。",
    "任何不是用户为了写报告而输入的自由文本：整个系统里唯一的自由文本是 `user_report.message`，只在用户按下「发送」后发出，且表单事先展示过。",
]

def render(lang):
    zh = lang == "zh"
    out = []
    w = lambda text: out.append(fill(text))
    if zh:
        w("# 隐私数据清单：每个客户端发送什么、服务器保存什么\n")
        w("这份清单回答三个问题：发送了什么、为什么需要、怎样去除了个人信息。每一条都标出了代码位置（`文件:行号`）；代码位置由 `scripts/telemetry/build_inventory.py` 在生成时查找，所以会随代码更新。\n")
        w("## 一句话\n")
        w("两个独立的开关（错误报告、使用统计），默认开启，关闭后该类内容不再发送。数据只发往开发者自己的服务器 `api.alkaidlab.com`（Cloudflare Worker），不经过广告网络，不出售。除用户亲手写的反馈外，没有任何自由文本；名称、路径、地址、账号在发送前就不在载荷里。")
        w("\n服务器自己不保存 IP 地址，只保存 Cloudflare 给出的国家代码；Workers 日志已关闭 @w_obs。连接本身必然经过网络，Cloudflare 作为网络服务商在传输层看得到连接的 IP，这一层不由我们控制，我们的代码不读取、不保存、不记录它。\n")
        heads = ["字段", "示例", "代码位置", "用途（回答什么问题）", "能否识别个人或泄露私密内容", "已做的清洗", "保留时间"]
        groups = [("envelope", "每个批次都带的信封"), ("usage", "使用事件（使用统计）"), ("experience", "体验事件（使用统计）"), ("error", "错误事件（错误报告）"), ("crash", "崩溃报告（错误报告）")]
    else:
        w("# Privacy data inventory: what each client sends and what the server keeps\n")
        w("This list answers three questions: what is sent, why it is needed and how personal details are removed. Every entry names the code (`file:line`); the code locations are found by `scripts/telemetry/build_inventory.py` when the page is generated, so they follow the code.\n")
        w("## In one paragraph\n")
        w("Two independent switches (error reports, usage statistics), on by default; when one is off, nothing of that kind is sent. The data goes only to the developers' own server, `api.alkaidlab.com` (a Cloudflare Worker), not through an advertising network, and is not sold. Apart from feedback a person writes by hand there is no free text anywhere: names, paths, addresses and accounts are not in the payload to begin with.")
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
