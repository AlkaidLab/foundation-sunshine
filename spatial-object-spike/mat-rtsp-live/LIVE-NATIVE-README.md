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

Full-session native acceptance passed on 2026-10-23 at 01:11 Asia/Hong_Kong,
using the first of the user's maximum two authorized joint tests. The bounded
20-second capture and 22-second live object source delivered 983 post-PLAY MAT
cycles / 60395520 bytes with identical publisher/client SHA256 and authenticated
EOS. The client decoded 588 real desktop H.264 frames at 640x360, with 19437 ms
of simultaneous audio/video reception. All six hook entries were restored,
all test processes exited normally and the pinned audiodg identity was retained.
No second native test was needed.

See `MAT-FULL-NATIVE-RESULT.txt` for exact evidence and limits. Private original
logs are in `build/mat-full-native-run01/`; the frozen metadata/log hashes are in
`build/mat-full-native-run01-completed.json`. The tested source commit is
`2755c69012beae70ef66c675ef27f891b05621f4`; subsequent documentation edits do not
imply a rebuild. This controlled-source pass does not verify game A/V timing,
long-running stability or supported Android HDMI/eARC Atmos playback.
