# RTSS pipeline FPS comparison

Open the control panel's **RTSS Control → Monitoring**, select **Pipeline FPS**
(链路帧率对照), and enable monitoring. Select **Target FPS** too to show the
requested stream rate. Existing saved metric selections are preserved.
RTSS's own game FPS remains a separate measurement beside Sunshine's OSD slot.

Example:

```text
Target FPS: 120
Link FPS: Src[VDD pub] 120.0 | Cap 110.0
Submit 120.0 | Out 120.0 | Repeat 10.0 fps
```

The host publishes these measurements in `/api/perf/current`, per session under
`frame_rates`. All five rates use the same elapsed-time window (at least one
second). A new display/channel starts a new window. Warm-up and unsupported
source measurements are shown explicitly; they never fall back to target FPS.
Polling continues to measure zero throughput during a stall.

- **Src[VDD pub]** counts driver frame-channel publications using producer
  sequence deltas, including publications skipped by the consumer. It does not
  measure frames received at the driver's IddCx input or frames dropped before
  publication. The sequence is observed when metadata is read, before texture
  acquisition, so it can reveal publication/consumer differences.
- **Src[WGC arrival]** counts frames obtained in `FrameArrived`, before replacing
  an unread frame in the mailbox. It cannot count frames Windows never supplies.
- **Src[DXGI update]** counts accumulated desktop image updates reported by DXGI,
  excluding pointer-only notifications. Multiple updates may coalesce into one
  returned image; this is an API update rate, not scanout or game-render FPS.
- **Cap** counts successful capture updates dispatched to this session, before
  the encoder consumes them. It excludes capture timeouts and replay delivery,
  but can include cursor-only captured updates. No pixel-content uniqueness is
  inferred. Other backends show source FPS as `N/A` while retaining stage rates.
- **Submit** counts successful encoder submissions, including repeated frames.
- **Out** counts complete encoded frames published to the streaming queue before
  transport handling. It includes repeats and does not measure successful
  network delivery, decoder throughput, or client display FPS.
- **Repeat** counts submissions reusing a prior capture (including startup
  replay/dummy submissions); it is a subset of Submit, not an additional stream.

For DLSS frame-generation diagnosis, compare RTSS game FPS with Src and Cap.
Src ≈ 120 / Cap ≈ 110 points toward capture delivery pacing or processing.
Src ≈ 110 means to inspect the upstream virtual-display/presentation path,
while accounting for the source metric's backend-specific limitations.
Cap ≈ 110 / Submit ≈ 120 / Repeat ≈ 10 exposes host repetition explicitly.
Use a moving scene and compare after the measurement window has warmed up;
OSD changes themselves can also generate desktop updates.
