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
- Report the effective backend after successful session initialization,
  when the hybrid analysis selection is known; omit capability-probe summaries.

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
fail the probe. The standalone harness flushes D3D11 submissions because it
has no encoder; this does not add a per-frame flush to the production path.

To verify the build without shader compilers, configure a separate build with
explicit non-existent `SUNSHINE_DXC_EXECUTABLE` and `SUNSHINE_FXC_EXECUTABLE`
paths. Building `d3d12_video_probe` must succeed; running it must report
`hdr_shader_unavailable` and exit 1, rather than failing configure or linking.
Backend-selection unit tests cover ordinary D3D11 fallback and strict failures.

## Recorded local evidence

- Windows 11 build 26200; AMD Radeon 780M, driver 32.0.31041.1004.
- 27/27 backend, ring, statistics, and telemetry unit tests passed.
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
