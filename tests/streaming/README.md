# Real transport validation

These tools exercise the actual client core and validate captured H.264 offline.
They do not render video and are not an end-to-end latency benchmark.

Build the PC core under test and optionally use the existing PC FFmpeg bundle:

```powershell
cmake -S tests/streaming -B .codex-build/adaptive-fec-live-client -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
  -DTRANSPORT_COMMON_C_ROOT=C:/Users/mohaha/source/repos/moonlight-qt/moonlight-common-c/moonlight-common-c `
  -DTRANSPORT_FFMPEG_ROOT=C:/Users/mohaha/source/repos/moonlight-qt/libs/windows
cmake --build .codex-build/adaptive-fec-live-client --parallel 3
```

The runtime client needs a real launch ticket, host versions, codec capability
mask and the launch's ephemeral AES key. Invoke it through an isolated fixture;
never point it at an unrelated active session. Its positional arguments are:

```text
transport_stream_client host rtsp-url app-version gfe-version codec-mask seconds feedback-enabled key-hex capture-prefix
```

Duration is bounded to 1–60 seconds and video capture to 100 MiB. The client saves
`.h264`, `.frames.csv` and JSON observation output. CSV records actual decode-unit
boundaries. After capture, validate using a decoder-enabled client FFmpeg:

```powershell
.codex-build/adaptive-fec-live-client/transport_decode_capture.exe `
  capture.h264 capture.frames.csv
```

The decoder rejects invalid or excessive inputs and outputs JSON. It requires
an actual H.264 decoder; Sunshine's encoder-only FFmpeg lacks one. In the tested
PC bundle there is no H.264 parser, so CSV packet boundaries are required. Keep
the original CSV, byte total and file hash: successful decoding alone does not
prove byte-for-byte integrity.

The phase-six local fixture and independent proxy scripts are recorded in
`.codex-build/adaptive-fec-runtime/manifest.json`. Each preserved experiment has
its own runtime result, packet actions, server ledger, policy receipts and
capture. They use dedicated loopback ports and preprovisioned test certificates.
The proxy only rewrites plaintext RTSP's video port; the video identity and ENet
feedback retain real AES-GCM authentication. It records actions after authenticating
ingress identity and before forwarding. It does not simulate a bottleneck or OS
partial submission.

To repeat reconciliation for a preserved impaired experiment, pass its directory:

```powershell
python .codex-build/adaptive-fec-live-test/reconcile_faults.py `
  .codex-build/adaptive-fec-live-test/post-rs-fix-impaired
```

Comparison covers only the complete successful-send prefix observed by the
proxy. The later ingress tail is explicitly excluded. Live fixture certificates,
private keys and launch secrets must remain local and are not source artifacts.
See [the validation record](../../docs/adaptive-fec-validation.zh-CN.md) for
results and the remaining acceptance gates.
