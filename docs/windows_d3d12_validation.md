# D3D12 analysis integration validation

## Scope

PR #872 implements an opt-in D3D12 HDR analysis backend. Capture, conversion,
and encoder input remain D3D11; `auto` still selects D3D11. This report does
not establish a performance improvement or satisfy the G1 multi-vendor gate.

## Integration fixes (2026-09-08)

- Merge `master` at `04fa14eb3d1365655e156bb3352fbbc209fafbe9`, preserving
  pre-encode filtering, capture contracts, and current HDR statistics.
- Generate the embedded DXIL header through one explicit CMake target shared
  by Sunshine and the standalone probe. Tests no longer treat an ungenerated
  header from another CMake directory as an ordinary source file.
- Match the current shader ABI: 20-byte group results, 1,044-byte final
  results, and a shared R16_FLOAT PQ-average texture beside each FP16 cell
  statistics texture. Bind both SRVs and the declared b3 constant buffer.
- Use the same CPU publication code for D3D11 and D3D12, including the PQ
  average, HDR10+ distribution, near-black statistics, and sample sequencing.
  Ignore late results rather than publishing older metadata over a newer frame.
- Separate producer and completion fences, and use a non-shader-visible CPU
  descriptor heap for histogram clears. The debug layer detected the old
  clear-descriptor error even when the numerical result appeared correct.
- Keep each analyzer's slot state with its own resources, and serialize
  Wait/Execute/Signal submissions on the shared compute queue. A regression
  probe reproduced interference between two analyzers before this fix; both
  now retain all three slots and publish their own generations and statistics.
- Report the effective backend after successful session initialization,
  when the hybrid analysis selection is known; omit capability-probe summaries.
- Honor strict failures in analysis initialization and conversion, cancel a
  captured slot on mutex-release failure, and drain queued GPU work even when
  an analysis error has already marked the analyzer unavailable.
- Derive build availability from the generated shader sizes. Device bootstrap
  stays D3D11; only successful analysis initialization reports hybrid. Auto
  remains D3D11 even when the shaders are present.
- Submit the D3D11 producer batch explicitly after its successful fence signal.
  The asynchronous flush occurs at analysis cadence and its overhead remains
  part of the outstanding performance gate.

## Reproducible checks

Implementation responsibilities are separated as follows:

- `d3d12_hdr_analysis.cpp`: analyzer lifecycle, ring submission and readback.
- `d3d12_hdr_analysis_resources.cpp`: shared textures, descriptors and buffers.
- `d3d12_hdr_analysis_pipeline.cpp`: shader pipelines and command recording.
- `d3d12_hdr_analysis_retirement.cpp`: producer/compute completion and deferred
  resource release after an error or timeout.
- `display_vram_capture.cpp`: capture backends, image lifecycle and cursors.
- `display_vram_shaders.cpp`: D3D11 shader compilation and shared shader catalog.
- `hdr_analysis_result.cpp`: common D3D11/D3D12 metadata decoding; its header
  owns the shared GPU result ABI without depending on either graphics API.

In a Windows UCRT64 development environment with repository dependencies:

```powershell
cmake -S . -B build -G Ninja -DBUILD_DOCS=OFF -DBUILD_TESTS=ON -DBUILD_WEB_UI=OFF
cmake --build build --target sunshine d3d12_video_probe video_pipeline_telemetry_unit_tests
ctest --test-dir build -R '^video_pipeline_telemetry_unit_tests$' --output-on-failure
.\build\tests\d3d12_video_probe.exe --debug
```

The probe checks a synthetic golden result, including the PQ sum and histogram
count, then submits 512 frame-dependent fixtures through the three-slot ring.
It deliberately stalls compute while D3D11 fills all slots: no result or free
slot may appear, and the completion fence must not advance until compute is
released. Subsequent results must match their source frame. Debug-layer errors
fail the probe. Two simultaneous analyzers then fill separate rings with
different statistics to check ownership and generation isolation. The
standalone harness does not flush D3D11 or run an encoder: it relies entirely
on the analyzer's production submission path. Removing the harness flushes
also passed on this AMD driver before the explicit production flush was added;
the potential submission stall was not reproduced locally.

The failure probe blocks the D3D11 producer, writes a snapshot, and submits a
cancelled slot. This fails after producer Signal/Flush and before the compute
queue Wait. Destroying the analyzer must retain its resources while the producer
is blocked; releasing the gate must return the pending-retirement count to its
baseline without debug-layer errors. This passed on all three AMD enumerations.
The companion case blocks the compute queue after a valid submission and checks
the same retention/release behavior, independently of producer completion.

Teardown waits up to two seconds for private producer and compute fence markers.
A timeout transfers ownership to a retirement worker, which retains both devices,
the queue, fences and resources and does not access the destroyed display or
submit D3D11 commands. Resources are released only after both queues complete or
their respective devices report removal. If a producer marker cannot be queued,
resources remain held until device removal; if the retirement worker cannot be
created, they remain held until process exit. These rare failure cases prioritize
valid GPU resource lifetime and remain part of the outstanding fault/memory gate.

To verify the build without shader compilers, configure a separate build with
explicit non-existent `SUNSHINE_DXC_EXECUTABLE` and `SUNSHINE_FXC_EXECUTABLE`
paths. Building `d3d12_video_probe` must succeed; running it must report
`hdr_shader_unavailable` and exit 1, rather than failing configure or linking.
Backend-selection unit tests cover ordinary D3D11 fallback and strict failures.

## Recorded local evidence

- Windows 11 build 26200; AMD Radeon 780M, driver 32.0.31041.1004.
- Full Sunshine configure, compile, and link passed with the default warning
  policy; the resulting executable successfully reported its version.
- 33/33 backend, ring, statistics, and telemetry unit tests passed.
- All four selected CTest targets passed: the above suite, frame contract,
  pre-encode filter, and TrueHDR backend loader.
- All 20 CTest targets also passed before and after the structural extraction.
- Four decoder golden tests passed for empty results, the nine HDR10+
  percentiles/near-black coverage, invalid PQ values, and rounding overshoot.
- DXC SM6 and FXC SM5 compilation passed through the CMake-generated target.
- The modified analysis/probe code, `display_vram.cpp`, and `video_backend.cpp`
  compiled with GCC 15.2 and `-Werror`.
- Debug-layer probe passed on all three enumerated AMD adapter LUIDs; these
  are enumerations of the same vendor/device, **not three independent GPUs**.
- Golden result: 4,096 pixels, min 100, max 400, sum 819,200, PQ sum 2,048.
- Missing DXC/FXC: configure and probe build passed; runtime reported
  `hdr_shader_unavailable` on all three enumerations, as expected.
- Web UI production build passed with Node 24.4.0 / npm 11.4.2. These local
  versions are older than the repository's pinned Node 26.7 / npm 11.19;
  the pinned toolchain remains a CI check.

Enabling `BUILD_WERROR` across the entire project encountered a GCC 15.2
`-Wfree-nonheap-object` diagnostic in the unchanged `video_hdr_bitstream.cpp`.
Full application builds use the project's default `BUILD_WERROR=OFF`; the
targeted checks above retain `-Werror`. No project warning policy was relaxed.

## Real desktop capture and encode check (2026-09-08)

A local diagnostic linked the current production objects and called
`video::probe_encoders()` and `video::capture()` directly. It did not replace
the installed Sunshine service or use a Moonlight client. The active Zako
virtual output was 3840x2160 at 120 Hz, rendered by the AMD Radeon 780M.
Each successful case captured for about 15 seconds, scaled to 1920x1080,
requested 30 fps / 12 Mbps HEVC through standalone AMF, and decoded every
output frame with FFmpeg 8.0.1. Frame counts are smoke-test evidence, not a
performance comparison; these were live desktop sequences, not identical
recorded inputs. Temporary bitstreams were deleted after inspection.

| Requested backend | Capture | Transfer / conversion | Decoded frames | Result |
| --- | --- | --- | ---: | --- |
| D3D11 | DDAPI | SDR / compute | 439 | Pass, 8-bit BT.709 |
| D3D11 | DDAPI | PQ / compute | 437 | Pass, 10-bit BT.2020 + PQ |
| D3D12 strict | DDAPI | PQ / compute | 428 | Pass, hybrid analysis, no fallback |
| D3D12 strict | WGC | PQ / compute | 418 | Pass, hybrid analysis, no fallback |
| D3D12 strict | DDAPI | HLG / compute | 408 | Pass, 10-bit BT.2020 + HLG |
| D3D12 ordinary | DDAPI | PQ / pixel shader | 429 | Pass, explicit D3D11 analysis fallback |
| D3D12 strict | DDAPI | SDR / compute | 431 | Pass, D3D11 as intended for SDR |
| D3D12 strict | DDAPI | PQ / pixel shader | 0 | Expected refusal, `analysis_path_unavailable` |

Every successful case exited cleanly and had zero decoder errors. PQ streams
contained HDR10+ and HDR Vivid side data after the first frame; all 408 HLG
frames contained Vivid, with no HDR10+ side data. The HLG startup guard logged
three independent GPU samples before releasing its first Vivid-bearing IDR.
Runtime status reported active HDR analysis and scene metadata. This verifies
metadata presence and transfer/depth signaling, not numerical equivalence of
all metadata fields or visual fidelity on a receiving HDR display.

This check found and fixed a skipped-path bug: with compute conversion disabled,
strict D3D12 previously emitted 430 valid PQ frames while silently using D3D11
analysis. Output initialization now records `hdr_snapshot_path_unavailable`
when an enabled analysis path cannot create the shared statistics snapshot.
Ordinary mode continues on D3D11 with an explicit reason; strict mode rejects
the session with zero encoded frames. Capability probes and SDR sessions retain
their existing behavior. The normal D3D12 PQ case, ordinary fallback, strict
refusal, and SDR case above were rerun after this fix. Full build and all 20
CTest targets passed again. WGC and HLG cases were run immediately before it.

HDR was temporarily enabled for these tests and restored to its original
disabled state afterward. The installed Sunshine service processes remained
running with their original process IDs. The separate capture harness, build
script, runner, logs and JSON summaries remain local validation artifacts.

## Controlled local performance check (2026-09-08)

**Historical exploratory result.** The harness used for this first check did
not reproduce production streaming timer-resolution and CPU-priority requests.
Its host/packet timing must not be used as a production baseline. The corrected
experiment below supersedes that interpretation; the original measurements
remain recorded here for auditability.

**No stable performance advantage was demonstrated on this AMD Radeon 780M.**
The uninstrumented D3D12 host-call P95 median was 0.094 ms (0.78%) higher
than D3D11, within the observed run-to-run variation. DXGI local process
usage increased by 117.65 MiB. This result does not justify changing `auto`
or passing the G1 gate.

A Release diagnostic linked the production converter and standalone AMF HEVC
encoder, using two pre-uploaded deterministic 3840x2160 scRGB FP16 tiled
textures. The textures alternated every 60 frames and had the same fixture
checksum in every run. Output requested 4K60 PQ at 30 Mbps. The harness excluded
desktop capture, transport and client presentation; it did not display or
capture user content. D3D12 strict mode reported active hybrid analysis in
every D3D12 case. The GPU debug layer was disabled for performance runs.

Each case used 300 warm-up frames and 2,000 measured frames. Analysis off,
D3D11 analysis and D3D12 analysis were each repeated three times in rotating
order, first with diagnostic timing and then with production timing disabled.
Two preliminary timing-off controls were also retained. All 40,000 measured
frames across these 20 cases produced encoded packets; this is not a stream
dropped-frame or decode-equivalence test.

The primary comparison below disables all production GPU queries and raw
telemetry. Harness clocks and CSV output remain identical across modes.
Values are medians of the three per-run statistics. Host call covers conversion
and the encoder call; packet-ready measures time from entering conversion
until the encoded packet is returned. Neither includes network/display latency.

| Analysis | Host mean / P95 / P99 (ms) | Packet-ready P95 (ms) | Convert mean (ms) | DXGI local usage (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Off | 9.513 / 11.382 / 13.233 | 11.435 | 0.233 | 500.50 |
| D3D11 | 9.794 / 12.096 / 13.186 | 12.137 | 0.214 | 523.50 |
| D3D12 | 9.891 / 12.189 / 13.414 | 12.240 | 0.284 | 641.15 |

Uninstrumented host P95 repeats were 12.0955 / 11.6430 / 12.1348 ms for
D3D11 and 12.2363 / 12.1894 / 11.7275 ms for D3D12. The direction changed
in the third repeat. Process CPU consumption medians were 4.55% and 4.78%
of one CPU core respectively. DXGI usage is process allocation/accounting
on this integrated GPU, not dedicated physical VRAM or a peak-memory bound.

The separate instrumented matrix sampled every five frames, yielding 100
completed analysis GPU samples per run (300 per backend). D3D11 analysis
elapsed mean/P95 medians were 0.458/0.512 ms; D3D12 was 2.772/5.602 ms.
The D3D12 first-stage span accounted for most of this tail (P95 5.579 ms);
readback copy P95 was 0.032 ms. These queue timestamp spans include scheduling
and preemption, and cannot establish that the shader alone is that much slower.
The final D3D12 span also includes resource restoration. Query resolution is
outside the final timestamp. The old D3D11-only `total_gpu_ms` excludes D3D12
queue work and is now explicitly labeled `timing_scope=d3d11_queue_only`.

Dense timing plus raw logging increased mean host-call time by approximately
0.56 ms for D3D11 and 0.57 ms for D3D12 versus the uninstrumented matrix.
This includes queries, formatting, logging and scheduling effects; it is not
isolated CPU timestamp overhead. Consequently these instrumented host times
are not the primary performance result, and the plan's low-overhead timing
gate remains unproven. Default diagnostic sampling is much sparser (31 frames)
and raw logging is off. The odd default avoids phase locking against the
four-frame HDR analysis cadence, which could previously omit every analysis
sample with the 30-frame interval.

D3D12 timing is opt-in with `SUNSHINE_VRAM_TIMING=1`, using a query heap and
per-slot timestamps resolved into the existing asynchronous readback buffer.
No explicit flush or CPU fence wait was added for D3D12 timing. The follow-up
also forbids implicit flushes in the existing D3D11 timing poll. Query-heap creation
failure leaves analysis available without timing. `SUNSHINE_VRAM_TIMING_RAW=1`
adds per-sample logs, and `SUNSHINE_VRAM_TIMING_SAMPLE_INTERVAL` accepts 1–1000
(default 31; use an interval coprime with four to sample analysis frames).
Clock-calibrated submit-to-start estimates include producer waits and queue
scheduling; submit-to-poll includes polling cadence. They are diagnostics, not
proof of pure fence delay or a cross-queue critical path.

Pacing used `sleep_until` without forcing Windows timer resolution; recorded
start lateness P95 medians were about 18.5–19.3 ms for the enabled backends.
The test therefore does not establish smooth 60 fps delivery. It also excludes
game contention, NVIDIA/Intel, HLG and Dolby Vision RPU generation/injection.
HDR was restored to its original disabled state after both matrices. The
separate local harness, runner, raw CSV/logs, JSON summaries and full report
remain in the validation worktree under `benchmark-results/` and its parent.

After adding timing, the full build and all 20 CTest targets passed again.
The 32-test backend/ring/statistics/telemetry suite includes a regression for
classifying D3D12 analysis frames without reading D3D11 analysis queries.

## Measurement corrections (2026-09-08)

The corrected harness reproduces the timer and CPU scheduling requests from
`platf::streaming_will_start()` and the encode thread: maximum supported timer
resolution (0.5 ms on this host), `HIGH_PRIORITY_CLASS`, and
`THREAD_PRIORITY_ABOVE_NORMAL`. These settings are verified in each run and
released/restored afterward. It does not invoke the full streaming callback,
which also changes unrelated WLAN, mouse and vendor control-panel settings.
The first follow-up matrix without these requests was stopped and retained as
diagnostic evidence, separately from the final experiment.

Frame targets use a Windows high-resolution waitable timer, without spinning.
The predeclared start-lateness P95 gate is 1 ms. CPU/packet rows are preallocated;
logging sinks buffer records in memory and write after measurement. Twelve
untimed input frames after the measurement collect pending encoder outputs,
so a packet still in the asynchronous encoder is not mistaken for a lost frame.
The second deterministic texture is half as bright, rather than just rearranging
the same luminance distribution. Both textures remain offscreen.

Production timing queries now reuse completed D3D11 query sets (bounded by the
existing eight-sample pending limit) and pass `D3D11_ASYNC_GETDATA_DONOTFLUSH`
to every timing `GetData` call. Recycling clears all stage flags, and a regression
test verifies inactive stages are not read and the read flags are preserved.
This removes per-sample query creation and prevents telemetry polling from
changing command-buffer submission. Queries are never recycled before completion.

Windows HDR statistics now expose their optional producer `source_frame`,
separately from the readback `sample_sequence`. The benchmark checks monotonic
publication, coverage of the expected analysis sources, source age, and 18
scalar/percentile fields by producer frame. The field tolerance is
`max(1e-5, abs(reference) * 1e-5)`. This is an analyzer-statistics comparison,
not a bitstream-field or DV RPU equivalence claim.

The final protocol specifies six repeats of analysis off/D3D11/D3D12 in all
six order permutations, each with 300 warm-up and 2,000 measured frames.
Primary host timings disable production telemetry. A separate three-repeat
diagnostic matrix samples every nine frames with raw messages buffered by the
harness. Statistics compare whole runs; frame samples are not treated as
independent experiments. GPU timestamps remain elapsed queue spans, with the
previously documented boundary differences and possible preemption.

WPR GPU tracing was attempted but Windows rejected the profiling policy with
`0xc5585011`. A subsequent status check confirmed no active recording. The
GPU tail therefore remains unattributed to shader execution versus scheduling.
The user-enabled Zako display is retained; HDR is restored to its initial state.

### Corrected local result

All 24 cases returned all 48,000 measured packets. Every case passed the
predeclared pacing gate, with no harness deadline misses. Each enabled-analysis
case covered 500 measured producer frames with monotonic source and sequence
IDs. This does **not** imply that every numerical correctness check passed:
one D3D12 result failed the percentile comparison described below.

The table contains medians of six complete runs with telemetry disabled:

| Analysis | Host mean / P95 / P99 (ms) | Packet collection P95 (ms) | Start lateness P95 (ms) | DXGI local allocation (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Off | 0.565 / 0.809 / 1.130 | 17.660 | 0.506 | 524.24 |
| D3D11 | 0.660 / 0.873 / 1.074 | 17.743 | 0.507 | 547.24 |
| D3D12 | 0.684 / 0.968 / 1.169 | 17.795 | 0.513 | 664.89 |

The six paired D3D12-minus-D3D11 host P95 differences were +0.1369,
+0.0788, -0.0356, +0.5437, +0.2826 and -0.2701 ms. Their mean is
+0.1227 ms; an exploratory run-level paired bootstrap (10,000 resamples,
seed 872) gives a 95% interval of [-0.0763, +0.3261] ms. This does not
establish a stable acceleration, regression or equivalence. D3D12 adds
117.65 MiB of local DXGI allocation in this case.

Published statistics arrived one converted frame earlier with D3D12:
source-age P95/max was 4/4 frames versus D3D11's 5/5. This is analysis
freshness, not a demonstrated video-latency or perceptual improvement.
Packet latency here means observed collection at the production encode-call
cadence, not hardware completion, network delivery or client presentation.

The separate three-repeat GPU diagnostic has analysis elapsed-span P95
medians of 0.509 ms for D3D11 and 5.702 ms for D3D12, mostly in pass 1.
Instrumentation-on/off host and CPU deltas have inconsistent signs and are
confounded by separate collection periods. Query recycling, DONOTFLUSH and
buffered output remove known perturbations; they do not establish that the
remaining observer cost passes a low-overhead gate.

### Unresolved percentile discrepancy

Using one 500-source D3D11 reference, 8,500 cross-run producer-frame comparisons
across the other 17 enabled-analysis cases found two out-of-tolerance fields in one result: `d3d12-run9-timing1`,
source frame 392, P25 217.456177 versus 71.3556519 nits and P50 470.033203
versus 453.286896 nits. The same result was retained for four output frames.
The input had not switched scenes there. Other fields remained within the
predeclared tolerance; neighboring sources and the other runs matched.
The primary timing-off matrix had no such violations.

This is a failed correctness gate, retained without relaxing tolerance or
silently dropping the sample. The original run did not retain raw histograms,
so its cause cannot be inferred from the decoded percentiles alone. Follow-up
raw-histogram diagnostics are kept separate from the performance matrix.
A dedicated diagnostic binary adds buffered sparse histogram logging to the
shared decoder without modifying the production binary. One D3D11 reference
and five D3D12 follow-ups returned 12,000 measured packets. All 2,500 measured
D3D12 source results matched the reference raw histogram, its pixel-count sum,
and all 18 decoded fields. The anomaly did not recur; this does not explain
or clear the original failure. The D3D12 debug probe also passed afterward.

Both fixtures have eight equal-population histogram bins. Their P25 and P50
lie exactly at cumulative-count boundaries; even a small population change
can move the reported percentile to the next occupied bin. This explains the
sensitivity of the comparison, not the cause of the observed population or
result variation. No speculative barrier or shader change was applied.

The corrected experiment remains a fixed-input conversion/encoding test.
It excludes actual desktop capture, game contention, other GPU vendors, HLG,
DV RPU generation/injection and client presentation. No G1 acceptance or
production enablement is claimed. Local full build and all 20 CTest targets
passed after the query/provenance changes, including the new recycling test.

## Outstanding acceptance evidence

- Full client/server HDR streaming, transport and client presentation, including
  the native VDD capture backend, RTX HDR and resolution/HDR changes. The local
  DDAPI/WGC capture-to-encode smoke checks above do not cover these stages.
- NVIDIA/Intel hardware and the Windows-version matrix.
- D3D11/D3D12 comparison on the same real captured sequences, including PQ,
  HLG, scaling, bars, and bitstream metadata fields.
- 24-hour streaming, 500 stream reconfigurations, and injected device loss.
- Broader P95/P99, dropped-frame, game frame-time, and peak-memory comparisons;
  the fixed-input 4K60 result above is one local case and shows no stable gain.

The synthetic probe covers resource handoff and analysis output. It cannot
replace these end-to-end, stability, and performance checks.

## Review decisions

- Base-device initialization deliberately does not report hybrid. The new
  analysis-state helper and tests mark hybrid only after the analyzer starts;
  reporting it at device bootstrap would misreport SDR sessions.
- Removing `ALLOW_RENDER_TARGET` from the NV12/P010 sharing probes was tested
  and rejected: both previously passing capabilities became unavailable on
  all three AMD adapter enumerations. The existing flags are retained pending
  evidence for a compatible alternative on other drivers.
