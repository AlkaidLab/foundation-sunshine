# MAT diagnostics outside the protocol library

`MatDumpFile.c/.h` owns output paths, exclusive `.partial` creation, incremental
SHA, exact-count EOS, non-overwriting publication and diagnostic JSON status.
It is a consumer of common-c cycle/EOS callbacks, not part of AudioStream. The
optional `MatDumpPush` helper feeds SMAT through the existing finite receiver for
standalone tests; network frontends use `MatDumpCallbacks` without reassembly a
second time. Close the writer only after stream threads have joined.

`recording_receiver.h` owns raw offline-cycle sequence/lifecycle validation in a
separate state object. It reuses common-c carrier validation without adding raw
input modes to the network receiver. `mat-recording-check` reads an explicitly
named recording and delivers it to a mock consumer; it never plays audio or
acts as a Sunshine capture source.

Build the reviewed static common-c library, then run:

```powershell
./spatial-object-spike/mat-diagnostics/verify.ps1 -Output <fresh-directory> -CommonBuild <common-build>
<fresh-directory>/mat-recording-check.exe <recording.raw>
```

Verification generates carrier-shaped bytes, checks seven writer cases and
seven raw-input cases, exercises successful/truncated file input, and compiles
both actual AudioStream/RTSP diagnostic dump frontends with tool-owned storage.
It does not read a recorded native MAT fixture. The normal live acceptance also
runs this suite; its mock clients link the same reviewed common library.
The same suite runs the actual AudioStream receive/decoder threads over local
UDP: clean EOS succeeds, while missing END or the final data fragment must time
out without publishing a completed file. Use `-Python <path>` if needed.

The Android application/PR is not upgraded here. Earlier Android JNI/offline APK
experiments remain historical and require their pinned shared API (8a51e978 or
earlier). They are not the current acceptance entry point. No new phone, HAL,
HDMI/eARC, native capture or Atmos rendering result is claimed.
