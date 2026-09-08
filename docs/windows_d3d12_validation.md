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

To verify the build without shader compilers, configure a separate build with
explicit non-existent `SUNSHINE_DXC_EXECUTABLE` and `SUNSHINE_FXC_EXECUTABLE`
paths. Building `d3d12_video_probe` must succeed; running it must report
`hdr_shader_unavailable` and exit 1, rather than failing configure or linking.
Backend-selection unit tests cover ordinary D3D11 fallback and strict failures.

## Recorded local evidence

- Windows 11 build 26200; AMD Radeon 780M, driver 32.0.31041.1004.
- Full Sunshine configure, compile, and link passed with the default warning
  policy; the resulting executable successfully reported its version.
- 31/31 backend, ring, statistics, and telemetry unit tests passed.
- All four selected CTest targets passed: the above suite, frame contract,
  pre-encode filter, and TrueHDR backend loader.
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

## Outstanding acceptance evidence

- Real capture-to-encode HDR streaming and runtime fallback across WGC, DDAPI,
  and VDD, including RTX HDR and resolution/HDR changes.
- NVIDIA/Intel hardware and the Windows-version matrix.
- D3D11/D3D12 comparison on the same real captured sequences, including PQ,
  HLG, scaling, bars, and bitstream metadata fields.
- 24-hour streaming, 500 stream reconfigurations, and injected device loss.
- Repeatable P95/P99, dropped-frame, game frame-time, and memory comparisons.

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
