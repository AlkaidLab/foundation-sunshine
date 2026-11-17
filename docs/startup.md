# Start of the host (Sunshine.exe and its service)

Measured on a Windows test host, the log of 2026-11-04 12:53 (build 2026.1104.121813), times from the start of the service:

| time | what |
| --- | --- |
| 00.0 | `sunshinesvc` starts |
| 03.0 | the service loop's first pass: it waited 3 s before its first try (`WaitForSingleObject(stop_event, 3000)` at the top of the loop) |
| 04.4 | `Sunshine.exe` writes its first line |
| 04.56 | nvprefs done |
| 04.56 → 06.05 | **1.49 s with no log line**: `platf::init`, `proc::init`, `input::init` (the HID enumeration of the virtual mouse ends it: "Found virtual mouse device") |
| 06.06 → 07.29 | the encoder probe, 1231 ms: nine encoders created and tested in a row |
| 07.298 | nvhttp / nvhttps listening |

## Changes (branch `perf/host-start`, not built: needs the Windows build)

1. **sunshinesvc**: the first pass of the loop is at once (a console session that is not there yet is picked up at the next 3 s pass). Saves 3.0 s of every start of the service.
2. **Phase timers** (`[startup] <phase> <ms> ms (at +<ms> ms)`) for platf::init, proc::init, input::init and the probe: the 1.49 s gap gets its split on the first run.
3. **input::init beside the probe** (`std::async`): neither needs the other. The guard keeps its place in `main`, so the tear-down order is unchanged. A stream waits for both gates.
4. **nvhttp and rtsp open before the probe.** The handlers that read what the probe found wait on `startup::probe_gate()` (serverinfo, applist) or on both gates (launch, resume), at most 10 s: a stuck driver call answers `503 Sunshine is still starting` instead of hanging the client. rtsp answers only a session that /launch made, so it needs no gate of its own. The web UI already opened before the probe.
5. **`encoder_probe_cache.h`** (module and tests only, not wired): the record of the last successful probe under a key of version, GPU set with driver versions, the options that reach the probe and the capture backend.

## Why the cache is not wired yet

A hit would answer the probe in about 0 ms, but the record has to be checked afterwards, and the check is the probe itself: it writes the flags of the encoders that the servers read. It has to run on a private copy of `encoders` (`validate_encoder` writes `encoder_t::h264/hevc/av1`), and swap the result in only while no session runs. Until that is built and measured, the probe runs every start.

## Measuring

* Model (this machine, the real gates, the durations of the log): `startup_model.cpp`. It models the order of the start, not the machine.
* On Windows: start the service, read the `[startup]` lines; before/after on the same boot state. A cold start needs a reboot, a warm one a service restart.

## Measured on a Windows test host (2026-11-04, three cold starts of the service each, ms from the start request)

| | installed 2026.1104.121813 | perf/host-start |
| --- | --- | --- |
| Sunshine.exe created | 3023 | 17 |
| web UI answers | 4402 | 1348 |
| nvhttp port accepts | 5424 | 1377 |
| first serverinfo 200 | 5426 | 2348 |
| probe finished (ready to stream) | 5404 | 2347 |

The `[startup]` lines of the new build: **`platf::init` 1049-1057 ms** (the virtual mouse is found at its end), `proc::init` 0, **`input::init` 1 ms**, probe 1055-1116 ms. The guess of the first version was wrong:
the 1.5 s gap is `platf::init`, not `input::init`; moving `input::init` beside the probe changes nothing. What the build gained is the 3 s of the service's loop, the nvhttp port that opens before the probe
(1.0 s earlier) and a web UI that is not behind anything. Next: run what `platf::init` does for the virtual mouse (the HID enumeration) beside the probe.
