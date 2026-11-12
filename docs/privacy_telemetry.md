# Usage statistics, error reports and what leaves your computer (telemetry v1)

Foundation Sunshine sends a small amount of data to the developers' own server, `https://api.alkaidlab.com`, so that they can see how many people use it, how streams perform and what breaks. This page says exactly what, when, and how to turn it off.
No script of Google or any other third party is loaded by the Web UI or the control panel. Nothing is sent to a third-party analytics service by a client.

## Two choices, both on until you turn them off

| Choice | What it covers | Config key |
| --- | --- | --- |
| Usage statistics | counts of what is used and numbers about how streams perform (the `usage` and `experience` events below) | `telemetry_consent` (`on` / `off`) |
| Error reports | an error record with personal details removed: what failed and where (the `error` events below, and crash reports) | `telemetry_errors` (`on` / `off`) |

Both are on by default; the first run says so once and asks nothing. Turn them off in the control panel or the Web UI under **About › Privacy**, or by setting the key in `sunshine.conf`. The host applies the choice at once: the events of an off kind that were waiting (in memory and on disk) are deleted, and nothing of that kind is kept or sent afterwards. When both are off the install ID is deleted as well and nothing identifies this install.
A person who answered *off* to the earlier single question keeps *off* for error reports until they turn it on.

## Who sends what

The host is the only sender. The Web UI and the control panel hand their events to the host they belong to (`POST /api/telemetry/events`); the host checks them against the same allow-list, applies the two choices again, adds the identity and sends everything in batches. A browser behind a content blocker, or a network that cannot reach Google, therefore makes no difference.

## The events

Every event is a name and a few fixed words or numbers. Names that are not on this list, and properties that are not listed for a name, are dropped before anything is kept.

| Event | Properties |
| --- | --- |
| `app_start` | cold start (yes/no), start time in ms, update source (`mirror`, `github`, `other`) |
| `app_exit` | seconds the host ran |
| `stream_start`, `stream_end` | client kind (a fixed word such as `moonlight-macos`), client OS, video codec, width, height, frame rate, HDR, audio codec, transport, kind of application (`desktop`, `bigscreen`, `sparxie`, `steam`, `other`) and, for a Sparxie application, its game as a catalogue word such as `genshin`; the end adds the length in seconds and how it ended (`normal`, `error`, `timeout`, `kicked`) |
| `stream_quality` (one per stream) | frame rate (typical and low end), share of dropped frames, encode and capture time (typical and 95th percentile), network round trip, packet loss, bitrate, reconnects, stalls |
| `startup` | host start time, time to the first frame of a stream |
| `feature_used` | a fixed word naming a feature (`stream.hdr`, `web.apps_saved`, …) |
| `setting_changed` | which of a short list of settings changed (encoder, capture, codec modes, frame-rate and bitrate buckets, …) and the new value as a fixed word or a range such as `8000-15000` |
| `panel_page` | the page and the time until its first frame |
| `user_report` | a report you write in the error dialog: a category (bug, performance, compatibility, suggestion, other), the product and page, an error code, and one message made of your title, your description and a short summary of the error (cleaned, at most 2000 characters). It follows the error-reports choice. No contact address is sent; the full stack and the log lines stay on your computer |
| `error`, `log_summary` | component, a code, severity and a count (no text: the code is a hash of the fixed start of the log line, so the same error is one code); once an hour, how many warnings, errors and fatal errors the host logged |

The list of events and properties is the code in `src/telemetry/events.cpp`; the tests in `tests/unit/test_telemetry.cpp` check it. **About › Privacy** explains what is sent and what is not for each switch and opens "See what will be sent": the records waiting for the next send together with the fields every batch carries, exactly as they leave (`GET /api/telemetry/preview`); the counters are at `GET /api/telemetry/status`. The complete list of fields, with the code that sends each, why it is needed, how personal details are removed and how long it is kept, is [privacy-data-inventory.md](privacy-data-inventory.md) ([中文](privacy-data-inventory.zh.md)).

With every batch go: the version, the update channel, the operating system and its version, the CPU architecture, the GPU vendor (`nvidia`, `amd`, `intel`), the language, and two random numbers (below).

## What is never sent

Names or text you typed; the name of your computer, your account, your devices or your clients; file paths; IP or MAC addresses; the titles of your games and applications (a game is at most a catalogue word such as `genshin`); any free text. The text of an error is not sent: a log line or an exception message can hold what a person chose or typed. The only free text is the report you write yourself (`user_report`), after the form has shown you exactly what leaves; it is cleaned in your browser, again in the host and again on the server, and cut to 2000 characters.
The server keeps only the country code of the connection (from Cloudflare), not the address.

## Identity

`install_id` is a random UUID (version 4, RFC 9562), made once for this install and kept in `<config folder>/telemetry/install_id`. It is not derived from your hardware, name or account. `session_id` is a new random UUID at every start. Neither can be traced back to you by itself. Turning both choices off deletes the install ID; turning one on again makes a new one.
`GET /api/telemetry/identity` hands the install ID to the control panel and the crash reporter, and answers 403 while both choices are off.

## Sending, offline and limits

Events wait in memory and are written to `<config folder>/telemetry/queue` (at most 100 files and 1 MB; the oldest go first), then sent at least every 30 seconds in batches of at most 500 events and 256 KB. A failure waits 30 seconds, then doubles up to one hour; the server's `Retry-After` is obeyed; at most 15 requests a minute are made. Sending runs on its own thread and never makes a stream wait.
The endpoint can be changed with `telemetry_endpoint` (for a test server).

## What the server does with it

It stores per-install records (so that daily, weekly and monthly users can be counted) and the numbers above for reports to the developers. Events older than a week or from the future are given the time of arrival. The server drops anything not on the list.

## Crash reports

A crash report is a small description (the module file name and address where it happened, the error code, the version) and, when error reports are on, a dump of the crash. The dump is the smallest that still shows the call stack: no heap, no data segments, no handle data, only the pointer-sized values of each stack, with module paths filtered. Before it is sent, the account name in any `\\Users\\`, `/Users/` or `/home/` path and the user and computer names are overwritten in place. The Panel and `sparxie-launch` do the same. The dump is deleted from the server after 90 days. See [crash-reports.md](crash-reports.md).
