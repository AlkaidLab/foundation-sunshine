# Privacy data inventory: what each client sends and what the server keeps

This list answers three questions: what is sent, why it is needed and how personal details are removed. Every entry names the code (`file:line`); the code locations are found by `scripts/telemetry/build_inventory.py` when the page is generated, so they follow the code.

## In one paragraph

Two independent switches (error reports, usage statistics), on by default; when one is off, nothing of that kind is sent. The data goes only to the developers' own server, `api.alkaidlab.com` (a Cloudflare Worker), not through an advertising network, and is not sold. Apart from feedback a person writes by hand there is no free text anywhere: names, paths, addresses and accounts are not in the payload to begin with.

The server does not store IP addresses, only the country code Cloudflare supplies; Workers Logs are off `worker:wrangler.toml:12`. The connection itself necessarily crosses the network and Cloudflare, as the network provider, sees the connecting address at the transport layer; that layer is not ours, and our code does not read, store or log it.

## Sent with every batch (the envelope)

| Field | Example | Code | Purpose (which question it answers) | Could it identify a person or reveal private content? | Scrubbing applied | Retention |
| --- | --- | --- | --- | --- | --- | --- |
| install_id | `3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11` | `src/crash/crash_core.cpp:609` `src/crash/crash_core.cpp:593` | Count daily, weekly and monthly users; group one install's errors together. | No: random UUID v4 made once, not derived from hardware, name or account. | Stored only as install_hash (first 32 hex of SHA-256) `worker:src/crypto.ts:19`; the raw id is never stored. | activity, sessions: 400 days; events: 180 days. |
| session_id | `9a1c6b0e-52d4-4f1a-8e3b-7c2d5e9f0a13` | `src/telemetry/events.cpp:612` | Tell one run of the program from another (crash-free sessions). | No: new random UUID at every start. | Stored with the session summary. | sessions: 400 days. |
| product, channel, os, arch | `host`, `stable`, `windows`, `x64` | `src/telemetry/events.cpp:612` `src/telemetry/telemetry.cpp:96` | Split every number by product, release channel, OS and CPU. | No: fixed words. | Fixed lists in the sender and the Worker `worker:src/schema.ts:165`. | As the record that carries it. |
| version | `2026.1112.0.杂鱼` | `src/telemetry/events.cpp:588` | Know which release has the problem. | No: the program's own version. | Only letters, digits and `. _ + -` space are allowed; anything else becomes empty `src/telemetry/events.cpp:588`; the Worker applies the same pattern `worker:src/schema.ts:165`. | As the record that carries it. |
| os_version | `10.0.26100` | `src/telemetry/telemetry.cpp:211` `src/telemetry/events.cpp:597` | Find problems that only occur on one Windows build. | No: the OS build number, shared by millions. | Only letters, digits and `. _ + -` space, at most 48 characters `src/telemetry/events.cpp:597` `worker:src/schema.ts:165`; free text is no longer scrubbed and passed on, it is refused. | sessions: 400 days. |
| gpu_vendor | `nvidia` | `src/telemetry/telemetry.cpp:125` | Find problems that only occur with one graphics vendor. | No: one of four words (nvidia, amd, intel, apple) or `other`; no device name, no driver string. | The vendor is reduced to a word `src/telemetry/telemetry.cpp:125`; the Worker takes only short lower-case tokens. | sessions: 400 days. |
| locale | `zh-CN` | `src/telemetry/events.cpp:604` | Know which languages need attention. | No: a language tag. | Must match a language-tag pattern, else empty `src/telemetry/events.cpp:604` `worker:src/schema.ts:165`. | sessions: 400 days. |
| country (added by the server) | `CN` | `worker:src/http.ts:67` | Know where users are, for language and network decisions. | Weak: a country, never a city or an address. | Cloudflare's country code is the only place information kept; the IP address is not read, stored or logged by the Worker; Workers Logs are off `worker:wrangler.toml:12`. | sessions, activity, crash_reports: 400 days. |

## Usage events (usage statistics)

| Field | Example | Code | Purpose (which question it answers) | Could it identify a person or reveal private content? | Scrubbing applied | Retention |
| --- | --- | --- | --- | --- | --- | --- |
| app_start: cold, start_ms, update_source | `true`, `1800`, `mirror` | `src/telemetry/events.cpp:301` | How long start takes; where updates come from (Mirror酱 or GitHub). | No: a boolean, a number, one of three words. | Allow-list of types and ranges `src/telemetry/events.cpp:301`. | events: 180 days. |
| app_exit: uptime_s | `3600` | `src/telemetry/events.cpp:302` | How long the program runs; whether a session ended normally. | No: a number. | Range 0 to one year. | events: 180 days. |
| stream_start / stream_end: client_kind | `moonlight-macos-enhanced` | `src/telemetry/events.cpp:303` `src/telemetry/stream_stats.cpp:92` | Which client families are used, to decide where to invest. | No: one of the six families the host itself recognises, else `other`. A name a person gave a device is never passed (this was a free catalogue id before). | Allow-list in the sender `src/telemetry/stream_stats.cpp:92` and an enum in the Worker `worker:src/schema.ts:53`. | events: 180 days. |
| stream_start / stream_end: client_os, codec, width, height, fps, hdr, audio_codec, transport | `macos`, `hevc`, `2560`, `1440`, `120`, `true`, `opus`, `altp` | `src/telemetry/events.cpp:303` `src/telemetry/stream_stats.cpp:122` | Which formats and resolutions people stream; whether the ALTP transport is used. | No: fixed words and numbers. | Enums in the sender `src/telemetry/events.cpp:303` and in the Worker `worker:src/schema.ts:53`; numbers clamped. | events: 180 days. |
| stream_start / stream_end: app_kind | `sparxie` | `src/telemetry/stream_stats.cpp:56` | Desktop, big-screen, Sparxie, Steam or other: which kinds of use matter. | No: five words. The application's name and command are read to choose the word and are not sent. | `src/telemetry/stream_stats.cpp:56` | events: 180 days. |
| stream_start / stream_end: sparxie_game | `genshin` | `src/telemetry/stream_stats.cpp:102` | Which Sparxie games are used (catalogue ids only). | No: a game of the Sparxie catalogue (nine ids) or `other`; a typed or unknown value becomes `other`. | `src/telemetry/stream_stats.cpp:102` and an enum in the Worker `worker:src/schema.ts:53`. | events: 180 days. |
| stream_end: duration_s, result | `600`, `normal` | `src/telemetry/events.cpp:307` | Total streaming time (the figure behind "minutes streamed"); how streams end. | No: a number and one of four words. | `src/telemetry/events.cpp:307` | events: 180 days; sessions keep the sum 400 days. |
| feature_used: feature | `stream.hdr`, `web.app_added`, `panel.pair_open` | `src/telemetry/events.cpp:308` `src/telemetry/service.cpp:220` | Which features are used at all. | No: our own ids. A page may name a feature only with an id of its own product (`web.` or `panel.`) `src/telemetry/service.cpp:220`. | Lower-case token `src/telemetry/events.cpp:308`; prefix check for pages `src/telemetry/service.cpp:220`. | events: 180 days. |
| setting_changed: key, value | `max_bitrate`, `15000-30000` | `src/telemetry/events.cpp:309` `src/telemetry/events.cpp:460` | Which settings people change, and to what kind of value. | No: a key from a list of 27 and a value that is a word from a list, or a range bucket. Free values such as paths, names and addresses are not in the list. | `src/telemetry/events.cpp:460`, and the Worker repeats the list and the value formats `worker:src/schema.ts:61`. | events: 180 days. |

## Experience events (usage statistics)

| Field | Example | Code | Purpose (which question it answers) | Could it identify a person or reveal private content? | Scrubbing applied | Retention |
| --- | --- | --- | --- | --- | --- | --- |
| stream_quality (per stream) | `fps_p50 119`, `encode_ms_p95 6`, `loss_pct 0.1`, `reconnects 0` | `src/telemetry/events.cpp:332` | Is streaming smooth? Frame rate, delay, loss, stalls by version and codec. | No: numbers. | Clamped numbers rounded to one decimal `src/telemetry/events.cpp:332`. | events: 180 days; AE points per Cloudflare's retention. |
| startup: host_start_ms, first_frame_ms | `2100`, `450` | `src/telemetry/events.cpp:351` | How long until the host is ready and until the first frame. | No: numbers. | Clamped. | events: 180 days. |
| panel_page: page, time_to_content_ms | `apps`, `180` | `src/telemetry/events.cpp:352` | How long a page of the panel takes to show its content. | No: a route id of our own and a number. | Token and range. | events: 180 days. |

## Error events (error reports)

| Field | Example | Code | Purpose (which question it answers) | Could it identify a person or reveal private content? | Scrubbing applied | Retention |
| --- | --- | --- | --- | --- | --- | --- |
| error: component, code, severity, count | `stream`, `e1a2b3c4`, `error`, `3` | `src/telemetry/events.cpp:310` `src/telemetry/service.cpp:393` | Which component fails and how often. The code is a hash of the fixed start of the log line (up to the first quote, bracket, colon or digit), so the same error is one code. | No: no message. Before this fix an error carried a scrubbed log line of up to 200 characters, which could still hold a device or game name; now the line stays on the computer. | The message property is gone from the allow-list in the sender `src/telemetry/events.cpp:310`, from the pages `m3e:src/telemetry/events.js:57` `m3e:src/telemetry/send.js:181` and from the Worker `worker:src/schema.ts:61`; an old client that still sends one has it dropped. | events: 180 days. |
| log_summary: component, fatal, error, warning | `encoder`, `0`, `2`, `9` | `src/telemetry/events.cpp:326` `src/telemetry/service.cpp:393` | How many warnings and errors the host logs per hour, by component. | No: counts. The component is one of 24 fixed words or `host`. | `src/telemetry/service.cpp:393` | events: 180 days. |
| user_report: category, component, page, error_code, attachment, message | `bug`, `web`, `apps`, `E_LOST`, `att-2026-11/<sha256>.webp`, `the apps page stays empty` | `src/telemetry/events.cpp:318` `m3e:src/telemetry/report.js:140` `m3e:src/telemetry/report.js:127` | A report the person writes and presses Send on. Answers "what was broken for this person". | Yes, by nature: it is text a person typed. It is the only free text in the system. It is sent only when they press Send, after the form has shown exactly what leaves (title, description, a short summary of the error with its top stack lines and last events). | Scrubbed in the browser `m3e:src/telemetry/scrub.js:52` and again in the host `src/telemetry/events.cpp:153` and the Worker `worker:src/scrub.ts:39`; at most 2000 characters; no contact field exists; the attachment is only a reference the developer panel returned. | events: 180 days; the GitHub issue until the developers close it. |

## Crash reports (error reports)

| Field | Example | Code | Purpose (which question it answers) | Could it identify a person or reveal private content? | Scrubbing applied | Retention |
| --- | --- | --- | --- | --- | --- | --- |
| meta: product, version, os_version, time, uptime_s | `host`, `2026.1112.0.杂鱼`, `10.0.26100` | `src/crash/crash_core.cpp:302` | Which release crashes, and how long after start. | No. | Same checks as the envelope. | crash_reports: 400 days. |
| meta: exception_code, module, module_offset, frames[≤64] | `0xC0000005`, `sunshine.exe`, `0x1a2b3c` | `src/crash/crash_core.cpp:302` `src/crash/crash_core.cpp:231` `worker:src/crash.ts:24` | Where in the program the crash happens, so it can be found with the unstripped binary of the release. | No: a file name and an address inside our own binary. Directories are cut off in the sender `src/crash/crash_core.cpp:231` and again in the Worker `worker:src/crash.ts:24` (a path would hold the account name). | File name only; hex offsets; the code is a fixed shape. | crash_signatures: 400 days. |
| signature | `ab…ab` (64 hex) | `src/crash/crash_core.cpp:302` | Group identical crashes. | No: SHA-256 of the first eight frames' module names and offsets. | — | crash_signatures: 400 days. |
| dump (host, Panel, sparxie-launch) | a file of tens to a few hundred KB | `src/crash/crash_win.cpp:186` `src/crash/crash_win.cpp:191` `src/crash/crash_core.cpp:506` `src/crash/crash_upload.cpp:98` `panel:src-tauri/src/crash/win.rs:210` `panel:src-tauri/src/crash/redact.rs:90` `tools/sparxie-launch/CrashReport.cs:302` `tools/sparxie-launch/DumpPrivacy.cs:87` | Lets the developers read the stack of a crash they cannot reproduce. | Reduced to the least possible. Before this fix the host and the Panel asked for "indirectly referenced memory" (the heap behind every stack pointer: TLS keys, pairing secrets, web UI sessions, clipboard text can live there). Now: no heap, no data segments, no handle data, only the pointer-sized values of each stack (MiniDumpFilterMemory), module paths filtered (MiniDumpFilterModulePaths), and a callback that keeps only module names and CodeView ids, thread records, contexts and stacks. On the computer, before upload, the account name behind `\Users\`, `/Users/`, `/home/` and the user and computer names are overwritten in place (ASCII and UTF-16) `src/crash/crash_core.cpp:506`. Possible residue: a pointer-sized value on a stack that happens to look like text; mitigated by the filter, not eliminated. It is why the dump is only sent with error reports on. | Dump type and callback `src/crash/crash_win.cpp:186`; byte redaction in three implementations with the same test vectors. | Release asset of a private GitHub repository, deleted after 90 days `worker:wrangler.toml:31`; at most 20 per signature, one per install and signature `worker:migrations/0001_init.sql:87`. |

## What the Worker stores

| Store | Columns / content | Code | Purpose | Retention |
| --- | --- | --- | --- | --- |
| D1 activity | day, product, install_hash, version, channel, os, country, update_source | `worker:migrations/0001_init.sql:4` | Count users per day. | 400 days `worker:wrangler.toml:35` |
| D1 sessions | install_hash, session_id, day, product, version, channel, os, os_version, arch, gpu_vendor, locale, country, first_ms, last_ms, events, streams, stream_s, errors, exited, codec | `worker:migrations/0001_init.sql:16` | One summary per run: length, streams, errors, crash-free rate. | 400 days |
| D1 events | day, t, install_hash, session_id, product, version, kind, name, props (JSON of the allow-listed props) | `worker:migrations/0001_init.sql:41` | Percentiles, top errors, panel time to content. | 180 days |
| D1 crash_signatures | signature, version, product, kind, exception_code, module, module_offset, frames, first_ms, last_ms, count | `worker:migrations/0001_init.sql:55` | Group crashes, mark new ones. | 400 days |
| D1 crash_reports | day, time_ms, install_hash, signature, version, product, kind, os_version, uptime_s, country | `worker:migrations/0001_init.sql:70` | Crash rate per release. | 400 days |
| D1 crash_dumps | signature, install_hash, tag, name, bytes, stored_ms (the dump itself is a release asset) | `worker:migrations/0001_init.sql:87` | Index of the stored dumps. | 90 days with the dump |
| D1 alerts, counters, report_state | alert key, title, body, labels, issue number; rate-limit counters; day of the daily report | `worker:migrations/0001_init.sql:98` `worker:migrations/0001_init.sql:114` | De-duplicate alerts and reports; stay inside the free tier. | 7 to 400 days |
| Analytics Engine | index1 = product; blobs: kind, name, version, channel, os, country, gpu_vendor, arch, install_hash and the string props; doubles: the numeric props | `worker:src/schema.ts:183` | Fast aggregates of high-volume points. | Cloudflare's retention |
| GitHub issues (private repo) | daily report (totals and top lists), crash alerts (signature, product, version, exception, module, frames), user reports (category, product, version, channel, os, os_version, country, where, install hash prefix of 8 characters, time, message in a code fence) | `worker:src/report.ts:35` `worker:src/crash.ts:133` `worker:src/userreports.ts:17` | The maintainers read reports and alerts. | until closed |

## Never sent

- Screen content, audio, keystrokes, files, clipboard content.
- The name of the computer, the Windows or macOS account, a device or a client; the name of a game or an application (a game is at most one of nine catalogue words).
- File paths, IP addresses, MAC addresses, ports, pairing identifiers, Steam, miHoYo or other account ids, Mirror酱 CDKs, passwords, tokens, cookies.
- Any free text the person did not write for a report: the only free text in the system is `user_report.message`, sent when they press Send, after the form showed it.

## How it is enforced

- The sender has an allow-list: an event name or a property that is not listed is dropped before it leaves the computer `src/telemetry/events.cpp:310` (unit tests in `tests/unit/test_telemetry.cpp`).
- The Worker checks the same list again; a property of the wrong shape is dropped, never "cleaned and kept" `worker:src/schema.ts:61`.
- The three scrubbers run the same test vectors: `tests/data/telemetry_scrub_vectors.json` (C++), m3e `test/scrub-vectors.json` (JS), Worker `test/scrub-vectors.json` (TS); `scripts/telemetry/check-scrub-vectors.sh` checks the three copies are identical. The vectors plant a fictional account name, computer name, paths, IPs, MACs, UUIDs, e-mail and tokens.
- The byte-level redaction of crash dumps has one implementation each in C++, Rust and C#, with the same test vectors.
- Consent: error reports and usage statistics are independent; the host checks before every send, and events from the pages are checked once more by the host `src/telemetry/service.cpp:220`.
- The person can read "what is sent and what is not" under About › Privacy and open "See what will be sent" for the exact next send `src/telemetry/telemetry.cpp:470`.
