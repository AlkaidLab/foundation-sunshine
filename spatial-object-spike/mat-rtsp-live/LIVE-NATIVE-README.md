# Full Sunshine live MAT / RTSP / video acceptance

`live_verify.py` uses an actual normally built Sunshine, isolated paired TLS
state, NVHTTP launch, encrypted RTSP and production `LiStartConnection`. The new
`live_session_client.c` explicitly requests live codec 7, consumes authenticated
MAT callbacks, computes stream SHA256 and decodes H.264 into memory with FFmpeg.
No hardware output capability is claimed by this diagnostic sink.

The source is the existing native plugin pre-submit object capture and independent
MAT encoder. A separately initiated ISpatialAudioClient generator submits live
objects on the explicit UUYC endpoint; no recorded media is read. The private
publisher logs the exact post-PLAY SHA256 after its bounded EOS acknowledgement.
The harness compares its count/SHA with the actual client, requires decoded video
frames and simultaneous audio/video reception, and checks native hook restoration.
This does not calibrate media timestamps or prove HDMI/Atmos playback.

`run-full-native.ps1` validates a frozen manifest and the selected audiodg identity,
deploys immutable native helpers, and invokes one bounded test. It never elevates
or retries itself. The Python host has its own config, state, certificates, port
and normal shutdown path. The installed Sunshine remains untouched. Host startup
uses a process-local ProgramData opt-out from NVIDIA preference recovery; audio,
display, driver and service settings are not changed. Video/audio payloads are
not persisted. Only metadata, hashes, isolated credentials and logs are saved.

The import from historical `verify.py` is restricted to `cert_pair`,
`check_bridge_absent` and `stop_own_host`; its finite-source main is never called.
The new configuration leaves `mat_dump_replay` empty and strict-live negotiation
rejects a missing native source rather than selecting a recording.

Pre-native checks on this machine:

* Real encoder-generated video over an anonymous pipe -> production H.264 decode
  callback: 120 frames decoded, no media files. The encoder-only host FFmpeg and
  decoder-enabled local FFmpeg are built as separate test processes.
* Isolated normal Sunshine startup, paired serverinfo/applist and normal exit:
  passed, no native attachment. Private evidence: build/mat-full-live-preflight-v1.
* Native publisher SHA oracle and delayed EOS acknowledgement regression: passed.
* Native callback, paired lifecycle and 1300-round IPC tests: passed.

The requested full-session native test has not yet run as of this preparation.
Its proposed scope is 20 seconds of capture, 22 seconds of live object emission,
one encrypted RTSP session and 640x360 H.264 at 30 fps. Native execution requires
the user's current explicit authorization. Existing eight-second loopback passes
are not full RTSP/video acceptance evidence.
