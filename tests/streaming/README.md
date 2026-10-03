# Real transport validation

These tools exercise the actual client core and validate captured H.264 offline.
They do not render video and are not an end-to-end latency benchmark.

The default client core is this checkout's pinned `third-party/moonlight-common-c`.
To test a different checkout, pass `TRANSPORT_COMMON_C_ROOT`. The optional decoder
requires the PC FFmpeg bundle; replace the example path with your local bundle,
or omit `TRANSPORT_FFMPEG_ROOT` to build only the runtime client:

```powershell
cmake -S tests/streaming -B .codex-build/adaptive-fec-live-client -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
  -DTRANSPORT_FFMPEG_ROOT=D:/path/to/moonlight-qt/libs/windows
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

Historical isolated experiments and their proxy/reconciliation scripts are kept
with local runtime records; they are not distributed by this source target.
A new live run must record its own launch identity, exact source and binary
versions, packet actions, server ledger, policy receipts and capture. Use dedicated
ports and preprovisioned test certificates. If a proxy changes plaintext RTSP's
video port, preserve the authenticated video identity and ENet feedback, and
record actions after authenticating ingress and before forwarding. Such a proxy
alone does not simulate a bottleneck or OS partial submission.

Independently reconcile source/parity/feedback bytes, deliberate drops and
reordering, receiver raw-loss and recovery counters, and actual decode-unit
boundaries. Successful negotiation or offline decoding is insufficient evidence
for the application control loop or QoE.

Comparison covers only the complete successful-send prefix observed by the
proxy. The later ingress tail is explicitly excluded. Live fixture certificates,
private keys and launch secrets must remain local and are not source artifacts.
See [the validation record](../../docs/adaptive-fec-validation.zh-CN.md) for
results and the remaining acceptance gates.
