# Graphics driver failures, the safe mode and the background encoder check

Some crashes and hangs of the host end inside the graphics driver, not in the host: an Intel driver that calls a null function pointer 0–6 s after the start (one
computer 11 times in a day), an NVIDIA driver that holds the encoder thread, an AMD runtime that fails during a stream. The host recognises these, says so in
plain words, protects the start from a crash loop and no longer waits for the driver while it checks the encoders.

## Recognising a driver failure

The crash handler (`src/crash/`) looks at the first frame that is not a runtime or system frame (the same frame the report signature starts at). When it lies in a
module of a graphics driver (`src/crash/gpu_driver.h`) it writes `<config>/crashes/gpu_driver_mark.json`:

```json
{"v":1,"vendor":"intel","module":"igd12umd64.dll","driver":"32.0.101.6325","phase":"startup","kind":"crash","time":1731889200}
```

* Modules: Intel `igd*`, `igxe*`, `igc*`, `igvk*`, `ze_intel*` …; NVIDIA `nvwgf2um*`, `nvd3dum*`, `nvEncodeAPI*`, `nvcuvid*`, `nvapi*` …; AMD `amdxx*`, `atiu*`,
  `atidxx*`, `amdvlk*`, `amfrt*` … (the complete tables are in `gpu_driver.h`; a unit test checks each entry). A crash in `d3d11.dll`, in the host, or in the system
  runtime is not a driver failure, whatever lies below it.
* `kind` is `crash` (a fatal exception, abort, terminate) or `hang` (a watched thread stayed busy too long: the stuck thread's top frame is looked at).
* `phase`: `startup` (before the encoder check), `probe` (the encoder check), `idle`, `streaming`.
* `driver` is the version Windows reports for the vendor, read once at the start (a mark written before that has none).
* Only the file name of the module is kept: no path, no account name, no address. The mark never leaves the computer; it is written whether or not error reports are
  on (the report itself still follows the error-report switch).

## Crash-loop protection

`startup_guard.json` in the config folder records how the last runs ended (`src/gpu_driver_guard.h`).

* A run is a **failed start** when it ended without a shutdown request and had not stayed up 10 s, or when it hung in the driver (the encoder check did not return
  within 45 s) and did not end by a shutdown request.
* **3 failed starts in a row** put the next start into the **safe mode**.
* In the safe mode the start skips the **encoder check** and the **display recovery** (`display_device::session_t::init(true)`): the two places a driver call has held
  the start up. Everything else works. A stream still checks the encoders when it is asked for (the launch path does so every time, with the existing guard that skips an
  encoder the previous process ended in).
* How long a run has stayed up is measured on the **monotonic clock** of the process, not on the date. A date that is set forward (a time sync after a start without a network)
  cannot make a run that fails within seconds look stable, and a date set back cannot keep a good run from becoming stable. The times written into the file stay wall-clock times and only
  document the run.
* The safe mode ends after **5 minutes of running**, when **a stream has ended**, or when the person asks for another try (below). The count then starts at zero; a try that
  fails again puts the next start back into the safe mode after one more failed start.

## The encoder check runs in the background

The check no longer runs on the main thread: the tray and the ready state do not wait for it. `GET /api/gpu-driver-guard` says `probe: "running"` until it is over
(`done`, `failed`, `skipped` in the safe mode); the Web UI and the Panel show "Checking encoders" meanwhile. A request that needs the result (serverinfo, launch,
resume) waits for it, at most 10 s, and is told that the host is still starting after that. The thread is watched (`encoder-probe`, 120 s: a hang report and a driver
mark when it stops inside the driver) and the end of the process waits for it only 3 s, so a call stuck in the driver does not hold the exit.
The `ctx.phase` word of the reports (docs/crash-reports.md) is `encoder-probe` while the probe runs, whatever phase the main thread has reached; in the safe mode the
`display-restore` and `encoder-probe` words are not set, because those steps do not run.

## What the person sees

* **Panel overview, Web UI overview, tray notification**: the same sentences. The components are in foundation-ui (`MDriverGuardNotice`, `foundation-ui/driverGuard`);
  the tray notification is made by the host from `src/gpu_driver_text.h`, and a Web UI test compares each line of that file with the foundation-ui texts.
* The notice names the vendor, the driver version, the module and the phase ("The Intel graphics driver crashed at startup"), says it is not a fault of Sunshine, and offers
  the vendor's official download page, and its support page for the case that the driver is already the latest version. Intel's download pages are split by hardware generation, so
  no single page covers them: its "update" button opens the Graphics category of the Intel download center, and a second button opens the Intel Driver & Support Assistant, which finds the right driver.
* **Once per driver version**: the notice is shown until it is dismissed; a dismissal is kept per `vendor:version` (`gpu_driver_notice.json`); the tray shows it once per version. A notice
  whose driver version differs from the one installed now is not shown (the person has updated); it comes back only when the failure happens again under the new version.
* The safe mode notice says what was skipped, when it ends, and has a **Retry encoder check** button.

## API

`GET /api/gpu-driver-guard` (authenticated like the rest of the Web UI API):

```json
{ "v": 1, "probe": "pending|running|done|failed|skipped",
  "safe_mode": { "active": false, "since": 0, "auto_exit_s": 0, "skipped": [] },
  "driver": null }
```

With a mark: `"driver": { "show": true, "vendor": "intel", "module": "…", "driver": "…", "phase": "startup", "kind": "crash", "time": 0, "key": "intel:…" }`.
`POST /api/gpu-driver-guard` with `{"action":"dismiss","key":"…"}` or `{"action":"retry"}` (leaves the safe mode and checks the encoders now, in the background); the answer is the new state.

## Tests

* `tests/unit/test_gpu_driver_guard.cpp` (`gpu_driver_guard_unit_tests`): module tables, the mark, the guard (counting, safe mode in and out, a manual retry, a hang, damaged files), the notice state.
* `tests/unit/test_background_probe.cpp` (`background_probe_unit_tests`): the order of the probe states, the gate, the bound on a request that waits for the probe, a probe that never returns and the end of the process.
* `tests/unit/test_crash.cpp`: the driver frame of a stack.
* `tests/tools/gpu_driver_guard_e2e.py` with `crash_mark_probe` (ctest `gpu_driver_guard_e2e`): real crashes in real processes. `SUNSHINE_TEST_DRIVER_MODULE=<file name>=<vendor>` stands a module of
  the program in for a driver module (tests only). With the host itself: `--exe <sunshine> --host --config-dir <scratch>`; `tests/tools/crash_e2e.py --driver-vendor intel` runs
  the existing crash modes against the host and also checks the mark (also with error reports off).
* `src_assets/common/assets/web/tests/driverGuard.test.js`: the Web UI home shows the component; the tray words equal the foundation-ui words.
