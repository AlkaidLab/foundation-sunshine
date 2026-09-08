# Windows video pipeline benchmark

This harness captures the reproducible host metadata and structured VRAM timing
summaries required by the D3D12 migration gates.

1. Start Sunshine with `SUNSHINE_VRAM_TIMING=1`.
2. Warm up, then record at least 2,000 frames. Keep the capture API, codec,
   resolution, frame rate, refresh rate, HDR transfer function, game scene, and
   encoder settings unchanged.
3. Save the Sunshine log, then produce an evidence file:

```powershell
.\scripts\video_pipeline_benchmark.ps1 `
  -LogPath .\sunshine.log `
  -Scenario d3d11-analysis `
  -Resolution 3840x2160 `
  -FrameRate 60 `
  -RefreshRate 120 `
  -CaptureBackend vdd `
  -EncoderBackend nvenc `
  -Codec hevc `
  -HdrTransfer pq `
  -RecordedFrames 2000 `
  -Repeat 1 `
  -OutputPath .\artifacts\d3d11-analysis-4k60-run1.json
```

Repeat the same workload for `analysis-off`, `d3d11-analysis`,
`d3d12-analysis`, `d3d12-fused`, and `d3d12-encoder` as those paths become
available. Run each primary scenario three times. Store at least one evidence
set per GPU vendor. The output includes the Windows build, adapter names and
driver versions, source log paths, workload context, and every periodic
P50/P95/P99 metric summary.

Use VDD for the 4K60, 4K120, 1:1, and scaled primary matrix, covering HDR
analysis off/on and every reachable PS, compute-direct, and compute-scratch
path. Also collect one WGC and one DDAPI timing smoke test. The three-run P95
spread should stay within `max(10%, 0.05 ms)`.

The timing mode is diagnostic and opt-in. It uses asynchronous timestamp queries
and never waits for a pending GPU result. Also compare instrumentation disabled
and enabled once; if the average CPU cost grows by more than 0.02 ms or GPU frame
time by more than 0.5%, reduce the sampling rate before collecting the baseline.
