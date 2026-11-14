# Crash, abort and hang reports (telemetry v1, `POST /v1/crash`)

Contract: the crash part of the telemetry v1 specification (see `docs/privacy_telemetry.md`). This page is what the code does.

## What is captured

| Where | Source | Kind | Dump |
| --- | --- | --- | --- |
| `sunshine.exe`, `sunshinesvc.exe` (Windows) | unhandled-exception filter; vectored handler for stack overflow and heap corruption only; `std::terminate`, `abort`, pure call, invalid CRT parameter; an uncaught C++ exception (GCC 0x20474343, MSVC 0xE06D7363) | `crash`, `abort` | minidump: threads, thread info, indirectly referenced memory, unloaded modules; no full memory; at most 20 MB (a larger one is redone without the referenced memory, then dropped) |
| `sunshine.exe` hang watchdog | `main-startup` (300 s), `video-broadcast` and `audio-broadcast` (20 s per packet), `video-encode` (15 s per frame) | `hang` | minidump taken while the stuck thread is suspended; the thread's own stack is the frames |
| `sunshine` (Linux) | SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT on a signal stack, `std::terminate` | `crash`, `abort` | none; frames only |
| `sunshine-gui.exe` (Panel) | panic hook (code location file:line is the first frame, never the message); native crash like the host; WebView2 `ProcessFailed` | `panic`, `crash`, `abort` | minidump for native crashes |
| `sparxie-launch.exe` | `AppDomain.UnhandledException` (+ dump), `TaskScheduler.UnobservedTaskException` (meta only) | `unhandled-dotnet` | optional dump |

The crashing thread only copies its context and wakes a worker made at start; the worker writes the dump, then a meta with the faulting
frame, then the full meta. Nothing on that path allocates. The process then ends with the exit code the default handling gives.

A hang report is written once per slot and process; the process is not touched.

## Files

`<config>/crashes/pending/<unix>-<product>-<kind>-p<pid hex>.json` (+ `.dmp`). `<config>` is `<install>\config` (the folder of `sunshine.conf`).
The Panel writes to `%LOCALAPPDATA%\AlkaidLab\Sunshine Panel\crashes\pending` and uploads those itself. `sparxie-launch` writes to the
host's folder when it can (the host uploads), else `%LOCALAPPDATA%\Sunshine\crashes\pending` (it uploads those). `SUNSHINE_CRASH_DIR` overrides
the folder of the Panel and `sparxie-launch` (tests).

Meta: `{v, product, [install_id], version, os_version, time (ISO-8601 UTC), uptime_s, exception_code, module, module_offset, frames[<=64], signature, kind}`.
Offsets are hex strings (`"0x1f4a"`): 64-bit values do not survive a JSON number. Frames after the first are return addresses.
Signature: SHA-256 over the first 8 frames, starting at the first frame that is not a runtime or system module (ntdll, kernelbase, ucrtbase,
libstdc++, System.\*, ...), each `module(lower case)+0x<offset lower hex>`, joined by `\n`. The host, the Panel and `sparxie-launch` share one
test vector.

## Consent, identity, upload

* Error reports follow the host key `telemetry_errors` (default on); without it an old `telemetry_consent = off` still means off. Turning it
  off removes every waiting report; nothing is written while it is off. The consent is re-read every 30 s.
* `install_id`: the host's `<config>/telemetry/install_id` (one line, UUID). The host creates it when absent; the Panel and `sparxie-launch` only read it.
* Upload: first pass 20 s after the start (`SUNSHINE_CRASH_UPLOAD_DELAY_S`), then every 30 min, on its own thread. Endpoint:
  env `SUNSHINE_TELEMETRY_ENDPOINT`, else the key `telemetry_endpoint`, else `https://api.alkaidlab.com`.
* 2xx (`ok`, `duplicate`, any `dump` value) and a 4xx other than 408/429: the report and its dump are deleted. 429 and 503: everything is kept
  and the pass stops. A network failure or another 5xx: kept for the next pass. At most 20 reports stay; older than 30 days go.
* A report the process itself wrote less than 10 s ago is not taken (a hang report is written while the process lives).

## Tests

`SUNSHINE_TEST_CRASH=null-deref|abort|terminate|throw|hang|upload-only` (host), `null-deref|abort|panic` (Panel), `throw|task|upload-only`
(`sparxie-launch`). `tests/tools/crash_e2e.py` runs each mode in a scratch copy against a local mock of `/v1/crash` and checks the pending
folder, the dump header, the upload and the deletion; it also checks consent off and a 500 answer. `tests/unit/test_crash.cpp` (25 cases),
`src-tauri/src/crash/{meta,upload}.rs` tests and `tools/sparxie-launch-tests/CrashReportTests.cs` cover the pure parts and the upload.

## Symbolisation

`scripts/release/archive-symbols.ps1` (called by the package recipe before `strip`) copies the unstripped `sunshine.exe`, `sunshinesvc.exe`,
`sparxie-launch.exe` and the Panel's `sunshine-gui.exe` (+ `.pdb`) to `C:\test-scratch\symbols\<version>\` with a `manifest.json`; the recipe's
Linux build host pulls them to `/scratch/release-symbols/<version>/`, where the storage mover archives them (add a rule for that folder to
`tiers.toml`: release artifacts, keep every version, move cold after 3 days). `scripts/release/symbolize.py meta.json --symbols <folder>` turns
the frames into function and file:line (`llvm-symbolizer` or `addr2line`; PE: ImageBase + offset, minus 1 after the first frame).
`sparxie-launch` frames are `(method token << 32 | IL offset)`; name the method with ILSpy or ildasm from the archived exe.

Sources: Microsoft Learn pages recorded in `docs/authority/source-index.tsv` as `dr-ms-crash-*`.
