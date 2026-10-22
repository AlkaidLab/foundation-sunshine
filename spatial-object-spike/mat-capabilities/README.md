# MAT session capability selection

Normal playback now prefers MAT and selects Opus before ANNOUNCE if either the
current output or host lacks compatible playback support. The local AUTO value
6 never appears on the wire: the selected codec is live MAT 7 or Opus 0.
Explicit replay diagnostic 5 and dump 4 retain their strict failure semantics.
The preferred contract is now mat-live-v1; see ../mat-live/README.md.
A host with finite replay support only now falls back to Opus.

The output needs Android 10+, a single verified HDMI/ARC/eARC media route that
explicitly advertises Dolby MAT, direct 192kHz/eight-channel IEC61937 support,
a bounded minimum buffer and a zero-byte IEC track on that actual route.
API 33+ uses media-attribute routing; API 29–32 also probes the media route with
10ms of PCM digital silence. Older or unverifiable routes select Opus.
TrueHD, JOC, PCM or a connected HDMI port alone do not qualify. No global volume
or surround/route setting is changed. Conservative fallback is expected when
vendor firmware cannot expose these capabilities.

After MAT selection, missing/mismatched confirmation is terminal. After Opus
selection, unexpected MAT confirmation is also rejected. The established MAT
sink pins the device and permanently rejects unknown/changed/non-HDMI routing,
including routing callbacks. Reconnect to negotiate again; no in-place downgrade.

From the Sunshine root, set JAVA_HOME, ANDROID_HOME and Python with NumPy, then:

    ./spatial-object-spike/mat-capabilities/verify.ps1 -Output ./build/mat-capabilities-new -Python <python-with-numpy>

Both common-c checkouts must be clean at the same commit. This runs 12 production
JNI/player groups, 18 output-policy cases, both 14-test CTest suites, Android NDK
checks and a diagnostic APK build, 21 actual-client RTSP capability scenarios
against a controlled peer, and the previous 15 strict rejection scenarios.
The RTSP frontend interrupts after handshake; its expected later start failure
does not prove media playback. A separate actual audio startup test checks final
codec queue/sequence/frame settings before hardware or media threads start.

Add -BuildFullClient -AndroidBuildRoot <short-dedicated-directory> to build a
complete two-ABI debug APK. First use needs an empty directory; reuse requires
its workspace marker. The verifier does not install or play anything.
Its source/artifact hashes and explicit untested flags are saved to proof.json.
Keep private recordings, proofs, keys and APKs out of public commits.

References: [MAT encoding](https://developer.android.com/reference/android/media/AudioFormat#ENCODING_DOLBY_MAT),
[direct playback](https://developer.android.com/reference/android/media/AudioManager#getDirectPlaybackSupport(android.media.AudioFormat,%20android.media.AudioAttributes)),
[media routing](https://developer.android.com/reference/android/media/AudioManager#getAudioDevicesForAttributes(android.media.AudioAttributes)).
