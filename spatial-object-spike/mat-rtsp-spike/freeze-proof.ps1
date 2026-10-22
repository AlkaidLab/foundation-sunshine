param([string]$Output="$PSScriptRoot/final-manifest.json")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New manifest file required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$files=@('src/audio.h','src/config.h','src/config.cpp','src/rtsp.h','src/rtsp.cpp','src/stream.h','src/stream.cpp','src/thread_safe.h','src/stream_mat_replay.inc',
 'src/spatial_audio/encoded_audio_source.h','src/spatial_audio/mat_replay_source.h',
 'spatial-object-spike/rtp-spike/build.ps1','spatial-object-spike/rtp-spike/sender_prelude.h','spatial-object-spike/mat-rtp-spike/main.cpp')
$files+=Get-ChildItem -LiteralPath "$root/src/spatial_transport" -File|ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')}
foreach($base in @('third-party/moonlight-common-c','spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core/moonlight-common-c')){
 foreach($name in @('AudioStream.c','Connection.c','Limelight.h','Limelight-internal.h','SdpGenerator.c','RtspConnection.c','RtpAudioQueue.c','PlatformCrypto.c','MatWire.h','MatDepacketizer.h','MatDepacketizer.c','MatDumpProfile.h','MatDumpFile.h','MatDumpFile.c')){$files+="$base/src/$name"}
}
$android='spatial-object-spike/moonlight-vplus/app/src/main'
foreach($name in @('java/com/limelight/Game.kt','java/com/limelight/nvstream/NvConnection.kt','java/com/limelight/nvstream/jni/MoonBridge.java','java/com/limelight/binding/audio/SmartAudioRenderer.kt','java/com/limelight/binding/audio/MatDumpRenderer.kt','java/com/limelight/preferences/PreferenceConfiguration.kt','jni/moonlight-core/Android.mk','jni/moonlight-core/callbacks.c','res/values/strings.xml','res/xml/preferences.xml')){$files+="$android/$name"}
foreach($name in @('build.ps1','build-tests.ps1','check-android.ps1','verify.py','unit.cpp','audio_client.c','freeze-proof.ps1','MAT-RTSP-ANDROID-DUMP-RESULT.txt')){$files+="spatial-object-spike/mat-rtsp-spike/$name"}
$sources=[ordered]@{};foreach($file in $files|Sort-Object -Unique){$sources[$file]=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash}
$artifacts=[ordered]@{}
foreach($file in @('build03/rtsp.obj','build03/stream.obj','build03/config.obj','build03/audio.obj','build03/host-common/libmoonlight-common-c.a','build03/android-common/libmoonlight-common-c.a','build03/mat-profile-unit.exe','build03/mat-audio-client.exe','rtp-build04/sunshine-audio-replay.exe','rtp-build04/moonlight-audio-dump.exe','rtp-build04/source-chunks.json','android-objects03/libmat-dump-check.so','proof03/proof.json','transport-regression02/proof.json','legacy02/proof.json','gradle-online.log','gradle-java-online.log')){
 $artifacts[$file]=(Get-FileHash -LiteralPath "$PSScriptRoot/$file").Hash
}
$result=[ordered]@{scope='No-device source/build/test snapshot. Host RTSP/stream object checks use the actual newer Android PyroWave header; NOT a linked Sunshine app or live RTSP/APK/device test.';hostCommon=(git -C "$root/third-party/moonlight-common-c" rev-parse HEAD);androidCommon=(git -C "$root/$android/jni/moonlight-core/moonlight-common-c" rev-parse HEAD);sources=$sources;artifacts=$artifacts}
[IO.File]::WriteAllText($Output,($result|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Host 'Source and successful build/test evidence frozen; no app deployment.'
