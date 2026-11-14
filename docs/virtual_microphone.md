# Virtual microphone

The host provides its own virtual microphone so that a client microphone in
Moonlight reaches Windows as a normal capture device, without VB-CABLE. It is
separate from the DualSense: it has its own USB identity, its own device model
and its own session.

## What Windows sees

| Property | Value |
|---|---|
| Product string | `Sunshine Virtual Microphone` |
| Manufacturer / serial | `Foundation Sunshine` / `SUNSHINEVMIC0001` (stable, so the endpoint keeps its identity) |
| USB identity | VID `1209`, PID `A1C0` (placeholder, see Open items) |
| Class | USB Audio Class 1.0: AudioControl interface, one AudioStreaming interface (alternate 0 empty, alternate 1 one isochronous IN endpoint) |
| Format | PCM, 1 channel, 16 bit, 48 000 Hz |
| Endpoint | isochronous IN, asynchronous, high speed, `bInterval` 4 (one 96-byte packet per millisecond), `wMaxPacketSize` 98 |
| Feature unit | master mute and master volume (-50 dB to 0 dB in 1 dB steps; the device applies the gain itself) |

The in-box Windows USB Audio driver binds to it; nothing is installed.

## Layers and the seam

```
 stream thread          write_pcm()                       src/virtual_usb/uac1_microphone.*
 (decoded client PCM) ─────────────► ring ──► resampler ──► device model (descriptors, control
                                                            requests, endpoint data, drift)
                                                                  │  virtual_usb::device
 ═════════════════════════ seam: src/virtual_usb/device.h ════════╪═══════════════════════════
                                                                  ▼  virtual_usb::transport
 src/platform/windows/usbip_device_transport.*   USB/IP PDUs, paced replies, attach/detach, sweep
                                                                  │
                                                       loopback_usbip_bridge ─► usbip.exe ─► usbip-win2 ─► Windows
```

* `virtual_usb::device` is what the model implements: identity, `control()`,
  `iso_packet_period()`, `iso_in()`, `bus_reset()`. It names no wire format and
  no host helper, and it compiles and is unit-tested on Linux.
* `virtual_usb::transport` is what a bus implements: `attach(device&)`,
  `detach()`, `status()`. `platf::usbip_device_transport` is the only
  implementation. A different bus can be added as a second implementation
  without changing the model.
* `platf::audio::virtual_microphone_t` (`virtual_microphone.*`) is the
  session: one model, one transport, the default endpoint guard and the
  one-per-host lock.
* `mic_redirect.cpp` chooses the backend and holds the status and the
  self-test.

Shared with the DualSense and not copied: `loopback_usbip_bridge`,
`usbip_host_controller` (attach, detach, stale-port sweep), usbip-core (PDU
parse and build), `paced_reply_queue` (`paced_reply_queue.h`, formerly
`ds5/ds5_paced_reply.h`, which now aliases it) and the default endpoint guard
(`audio_default_guard.*`, formerly inline in the DualSense client).

## Data path

1. `write_pcm()` copies mono S16 frames into a wait-free single-producer
   single-consumer ring (8192 frames). It never blocks; when the ring is full
   the incoming frames are dropped and counted.
2. The bus clock is the device clock. The transport holds each isochronous IN
   reply until the packets of the transfer have gone by (1 ms per packet), then
   asks the model for the packets, so the data is as fresh as the bus allows.
3. The two clocks differ. The consumer steers by a smoothed fill level: inside
   a dead band it copies samples unchanged; outside it resamples by linear
   interpolation with a ratio of at most +-0.5 percent.
4. A ring that runs dry answers with silence and refills to a start level
   (1.5 times the target fill) before playing again; the start level grows by
   240 frames after each underrun. A ring above the high-water mark
   (2880 frames) is trimmed back to 1.5 times the target. Silence while nobody
   is talking is not counted as an underrun.
5. While Windows has not opened the capture stream (alternate 0) nothing is
   queued: the frames are discarded on arrival and counted, so a recording
   never starts with old audio. Opening the stream (`SET_INTERFACE` alternate 1)
   also flushes what was queued before that moment.

## Lifecycle

* `virtual_microphone_t::start()` creates the device when microphone
  redirection starts (`init_mic_redirect()`); `stop()` removes it when the
  session ends. At most one exists per host (named mutex
  `Global\SunshineVirtualMicrophone`), and the session object that owns it is
  the only writer, so audio of an earlier session cannot reach a later one.
* `attach()` makes up to three attempts inside its budget (15 s). An attempt
  that stays silent for six seconds, or that `usbip attach` rejects, is torn
  down completely (helper, listener, stray import) and the next attempt starts
  on a fresh loopback port. Before each attempt and after each detach the
  transport waits (up to 3 s and 5 s) until Windows has removed the previous
  device node, because an attach that overlaps a removal can stall.
* `detach()` runs `usbip detach` while the device still answers and only then
  closes the listener; closing it first makes usbip-win2 treat the device as
  lost.
* After a crash the import remains in the usbip-win2 hub. The next
  `attach()` sweeps loopback imports with the microphone's identity (VID/PID,
  bus id, remote bus/device) that this process does not own, then attaches.
* `UNLINK` of a held isochronous reply is answered with `-ECONNRESET` and the
  reply is withdrawn; one already sent reports 0.
* The Windows default audio endpoints are recorded before the attach; any
  default that moves onto the device while the class driver publishes its
  endpoints is put back (conditional on the default being this device).

## Backend values and codes

`microphone_redirect_backend`: `auto` (default; USB microphone first, VB-CABLE on failure),
`usbip_experimental`, `vb_cable`, `disabled`.

| Reported by | Values |
|---|---|
| `active_backend` | `usbip_experimental`, `vb_cable`, empty |
| `state` | `absent`, `creating`, `enumerating`, `idle`, `host_capturing`, `remote_active`, `destroying`, `device_faulted` (USB microphone); `ready` (VB-CABLE) |
| `error_code` / `fallback_reason` | `MIC_USBIP_COMPONENT_UNAVAILABLE`, `MIC_USBIP_DEVICE_BUSY`, `MIC_USBIP_START_TIMEOUT`, `MIC_USBIP_CREATE_FAILED`, `MIC_USBIP_TRANSPORT_LOST`, `MIC_BACKEND_DISABLED`, `MIC_TEST_*` |

The self-test with the USB microphone (`virtual_microphone_selftest.*`) writes
a 440 Hz tone and listens for it on the Windows capture endpoint; it succeeds
only if the tone dominates the last second of the recording.

## What the Control Panel microphone panel must show

* Backend selector with the four values above; `auto` described as "USB
  microphone, then VB-CABLE". Remove the "experimental / Virtual Device Host /
  DualSense prototype profile" wording: the backend is the host's own device now.
* Status block from `/api/microphone/status`: `state` (translated), active
  backend, `component_available` (usbip-win2 present), `online`,
  `device_created`, `host_streaming` ("an application is recording"), buffered
  ms (`buffered_bytes / 96`), `underruns`, `dropped_frames`, `submit_errors`,
  `generation`, `error_code` with a translated message, `fallback_reason`.
* A test button using `/api/microphone/test`; the result names the backend
  that carried it.
* When the component is unavailable: say that the usbip-win2 host component is
  missing and offer VB-CABLE.
* A note that the Windows default devices are never changed.

## Open items

* The USB identity (VID `1209`, PID `A1C0`) is a placeholder. A release needs a
  vendor-assigned identity that cannot collide with real hardware; the
  default-endpoint guard, the stale-port sweep and the model constants use it
  in one place each.
* The default of `microphone_redirect_backend` is `auto`.
* The microphone model is a reusable Module candidate: `src/virtual_usb/`
  (device model, no OS or wire dependency) plus any transport that implements
  `virtual_usb::transport`. See the report for the Slot sketch.
