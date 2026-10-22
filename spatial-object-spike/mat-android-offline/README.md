# MAT offline and phone diagnostics

The offline verifier uses the companion Android checkout at
`spatial-object-spike/moonlight-vplus` and the private, SHA-locked recording
described in `docs/ATMOS-MAT-HANDOFF.md`. Both parents must use their committed
common-c gitlinks. Initialize recursive common-c dependencies and the host's
`third-party/moonlight-audio-haptics` before running.

Set `JAVA_HOME` to a JDK with JNI headers and `ANDROID_HOME` to an SDK with
platform/build-tools 36 and NDK 28.2.13676358. The scripts also accept explicit
`-JdkRoot` and `-SdkRoot` arguments. MSYS2 UCRT64 remains required at
`C:/msys64/ucrt64`. Python with NumPy is needed for the independent research
PCM oracle; pass `-Python` if it is not on PATH. The profile/lifecycle unit no longer needs a previous full
Sunshine build or its compile database.

From the Sunshine root, use a fresh output directory:

```powershell
./spatial-object-spike/mat-android-offline/verify.ps1 -Output ./build/mat-offline-new
```

This runs 12 production JNI/player groups with a fake final sink, both common-c
CTest suites, profile/dump lifecycle regression, Android native compile/link
checks and diagnostic APK build/signature inspection. Its proof describes
offline validation only. It does not build the complete Moonlight application.
The Java research PCM extraction is compared byte for byte with the existing
Python `mat-spike/analyze_mat.py` extraction before packaging.

## Explicit phone check

The diagnostic APK has a separate package (`com.limelight.matdiagnostic`) and
requests no permissions. Its built-in instrumentation additionally runs the
same JNI/player tests under Android ART. It copies the recording into the
app's cache temporarily, checks the real AudioTrack setup, submits **zero** MAT
bytes to that track, and records the available audio outputs, audio decoder
list and decoder creation results for MAT, TrueHD, E-AC-3/JOC and AC-4. This automated
check needs API 26+ because the reused test suite uses `java.nio.file`; the
manual diagnostic UI retains its API 24 minimum.

After completing offline verification, connect an authorized ADB device and
specify its serial explicitly:

```powershell
adb devices -l
./spatial-object-spike/mat-android-offline/check-device.ps1 `
  -OfflineBuild ./build/mat-offline-new -Serial <device-serial> `
  -Output ./build/mat-device-new
```

The script checks the APK against the offline proof before installing it, runs
instrumentation, and creates a separate device proof only on success. Failed
installation or instrumentation preserves its logs and does not produce a
passing proof. `INSTALL_FAILED_USER_RESTRICTED` requires the device owner to
allow installation; the script does not change device security settings.
If installation was completed separately through the phone's system installer,
pass `-SkipInstall`. The runner checks the installed APK's SHA256 against the
offline build before executing instrumentation, including when installation is
skipped.

A rejected sink setup can be the expected result for a phone speaker. An opened
track also does not prove usable HDMI routing, audible playback or Atmos
recognition. The MAT path requires a supported HDMI/eARC output; this diagnostic
does not add a phone-local MAT decoder or perform a network session. Manual
playback remains an explicit action in the diagnostic UI.

## Fixed-recording speaker audition

`-PlayResearchPcm` explicitly requests audible playback through the phone's
built-in speaker. It extracts the observed stable fullband PCM slots from this
one SHA-locked recording, sums them at quarter gain, duplicates the mono result
into stereo, and plays 170,880 frames at 48 kHz. The three bootstrap cycles keep
their duration as silence. The recording's LFE is required to be silent.
The original extraction is retained for its independent SHA comparison. For
audition only, a separate PCM array is peak-normalized to 8192/32768, then played
at AudioTrack volume 0.35; the device's global media volume is unchanged. Both
the extraction hash and submitted audition hash are recorded. This matters for
the quiet fixed recording (extracted peak 124/32768), whose original low gain
was not audible in the first phone test.
Route, partial writes, cancellation and drain are checked. No IEC carrier bytes
are submitted to the PCM track.

```powershell
./spatial-object-spike/mat-android-offline/check-device.ps1 `
  -OfflineBuild ./build/mat-offline-new -Serial <device-serial> `
  -Output ./build/mat-speaker-new -PlayResearchPcm
```

This is an explicit research audition, not a general MAT decoder or an Atmos
renderer. It does not interpret object coordinates, preserve spatial placement,
or connect to a live network session. A drained speaker track proves submission
and routing; a listener must confirm acoustic output separately. The generated
device proof records that distinction. By default each offline build generates
a fresh local signing key. Pass `-SigningKeystore <previous-build>/mat-debug.keystore`
to reuse this diagnostic's local key for an update. Android will reject upgrades
from a differently signed diagnostic, and the device script does not
automatically uninstall an app.

The APK embeds a private recording and uses a locally generated signing key.
Keep build directories, recording, APK and key out of public commits and PRs.
