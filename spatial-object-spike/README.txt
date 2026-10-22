Windows spatial object capture spike
Date: 2026-10-17 (Asia/Hong_Kong)

RESULT: PASS for controlled application-side, pre-render object capture.
System-wide audiodg capture and existing-game compatibility remain unvalidated.

What was actually tested
------------------------
Windows 11 Pro build 26300, physical NVIDIA F32D80U HDMI/DP audio endpoint.
NVIDIA HD Audio driver shown by Settings: 1.4.6.3.
The F32D80U spatial setting initially was Off. It was temporarily changed to
Windows Sonic for Headphones and restored to Off at the end of the experiment.
No new virtual audio driver, test signing, trusted certificate, or reboot was used.
Existing virtual endpoints were enumerated; the reported capture used F32D80U.

Pipeline
--------
capture_launcher.exe starts a separate spatial_probe.exe process, paused at an
explicit startup gate, then loads spatial_tap.dll into that controlled process.
The DLL hooks the source EXE's CoCreateInstance import and the returned public
COM interface calls:

  IMMDeviceEnumerator::EnumAudioEndpoints
  IMMDeviceCollection::Item
  IMMDevice::Activate(ISpatialAudioClient)
  ISpatialAudioClient::ActivateSpatialAudioStream
  ISpatialAudioObjectRenderStream::ActivateSpatialAudioObject
  ISpatialAudioObject::GetBuffer / SetPosition / SetVolume
  ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects

The DLL copies the object's final audio buffer immediately BEFORE the real
EndUpdatingAudioObjects invalidates its pointer. It copies the successful
SetPosition/SetVolume call values. Audio and metadata travel in a bounded,
local shared-memory ring to the collector process, which writes WAV and CSV.
The source emitter itself never serializes its own audio or coordinates.

Two native dynamic objects were rendered simultaneously through Windows Sonic:
  Object 1: 523.25 Hz, moving x/y/z, SetVolume 0.10
  Object 2: 783.99 Hz, different moving x/y/z, SetVolume 0.07
Each object's source samples have peak amplitude 0.2, BEFORE the renderer applies
its separate volume value. Captured audio is pre-render/pre-volume mono float32.

Measured capture (capture.log, verification.json)
-----------------------------------------------
Source PID 32852, collector PID 31924.
2 objects, 1000 captured blocks, 480000 total object frames, dropped=0.
Each object: 500 blocks, 240000 frames, 48000 Hz, exactly 5.000 seconds.
The 480000 total is the sum of two simultaneous 5-second objects, not 10 seconds
of wall-clock audio.

Independent verification compared every sample with the known test signal and
every recorded coordinate with the known trajectory at that buffer's frame time:
  Object 1: max sample error 7.451e-9; max coordinate error 2.976e-8
  Object 2: max sample error 7.451e-9; max coordinate error 3.020e-8
Measured frequencies: 523.2501 Hz and 783.9927 Hz.
Sample count, frame offsets, metadata sequence, volume, and separate-object hashes
all verified. objects.csv includes object ID, process ID, frame offset, QPC,
sample rate, frame count, x/y/z, volume, and WAV byte offset.
The object IDs are local capture IDs, not game engine source IDs.
QPC timestamps describe the time of interception, not DAC presentation time.

Controls
--------
control.log: same source with no DLL, normal exit, 0 captured objects/blocks.
capabilities.log: F32D80U Off -> maxDynamic=0.
capabilities-sonic.log: F32D80U Sonic -> maxDynamic=128.
capabilities-restored.log: F32D80U restored Off -> maxDynamic=0.
spatial-off-negative.log: dynamic stream creation on restored endpoint fails
with HRESULT 0x80070057. This sample requires a native spatial-enabled endpoint;
removing the Dolby dependency does not automatically remove Windows endpoint
capability gating.

audiodg audit
-------------
Standard-token OpenProcess VM/thread rights: access denied (error 5).
Elevated read-only audit: query, VM-read and VM-write/thread access checks succeed.
GetProcessInformation reports PROTECTION_LEVEL_NONE (0xFFFFFFFE).
Microsoft-only and Store-only binary signature mitigation flags are both zero.
This makes further audiodg investigation plausible on THIS machine, but does not
prove a working engine hook or describe all Windows builds/protection settings.
No DLL was loaded into audiodg, and no system process memory was modified.

Limits
------
- Proven interception is on the application's public spatial audio API calls.
  The private audio-engine object data path inside audiodg is a separate task.
- The launcher intentionally only starts our adjacent controlled probe; it has
  no command to attach to arbitrary applications or existing game processes.
- Startup gating allows hooks to be installed before the first COM call.
  Existing objects/mid-stream attachment, alternate COM activation paths, static
  object positions, HRTF/metadata interfaces, multi-stream lifecycle and protected
  or anti-cheat games have not been validated.
- This captures objects submitted by the application. Plain already-mixed PCM
  does not regain its original object positions. Encoded movie passthrough does
  not automatically become decoded objects.
- No ROAR rendering, multi-output clock synchronization, Sunshine integration,
  network protocol, Atmos re-encoding, or receiver interoperability was tested.
- This is bounded spike code, not production real-time code. Its COM hook uses a
  mutex/map, supports mono float32 and up to 2048 samples per object block, does
  not unload live hooks, and relies on process exit for complete cleanup.

Files and reproduction
----------------------
Run from this directory with the physical F32D80U spatial setting temporarily
enabled (Windows Sonic was sufficient for this experiment):

  .\build.ps1
  .\spatial_probe.exe --list
  .\capture_launcher.exe --dev 1 --seconds 5 --objects 2
  python .\verify_capture.py

Endpoint indexes can change. Inspect --list before using --dev.
Restore the endpoint spatial setting afterward.

Negative DLL control (uses a different metadata filename and preserves WAVs):
  .\capture_launcher.exe --dev 1 --seconds 3 --objects 2 --control

Main evidence:
  object-1.wav / object-2.wav: isolated pre-render object audio
  objects.csv: per-buffer positions, volume, sample offsets and timing
  verification.json: independent numerical validation
  capture.log / control.log: process, hook counts and drop counts
  audiodg-access.log / audiodg-access-elevated.log: access-check evidence

References:
https://learn.microsoft.com/en-us/windows/win32/coreaudio/render-spatial-sound-using-spatial-audio-objects
https://learn.microsoft.com/en-us/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobjectbase-getbuffer
https://learn.microsoft.com/en-us/windows/win32/api/spatialaudioclient/nf-spatialaudioclient-ispatialaudioobject-setposition
