# Hardware optical-flow validation

Requires an NVIDIA GPU and a driver providing `nvofapi64.dll` in System32.
Build the adapter with MSVC, then run `foundation_nvof_provider_smoke.exe 1920 1080`.
Repeat with `1922 1082` (partial edge grid cells) and `3840 2160`.

The diagnostic uses known diagonal translation and repeated resets. It verifies
median current-to-previous vector direction and full-resolution scaling, exact
zero output on reset, and preservation of the caller's viewport. It reports
outliers separately: a median pass does not mean every vector is correct.
The synthetic wraparound pattern produces edge mismatches; observed central
regions were correct while isolated edge vectors were much larger than the
known translation. Real moving-image comparisons remain required.

Append `--flow` to an adapter smoke invocation to select medium-quality optical
flow. This includes `--benchmark` and `image output_dir --pan` invocations.
Compare against the same build without `--flow`; do not reuse an older adapter's
output because first-frame reset behavior may differ.

The provider smoke's duration includes full CPU readback. Adapter smoke durations
also include readback unless benchmark mode is used. Neither measures capture,
encoding, network latency or Moonlight playback. Optical flow remains opt-in.
