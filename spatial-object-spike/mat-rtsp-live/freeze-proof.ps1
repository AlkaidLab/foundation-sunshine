param([string]$Output="$PSScriptRoot/final-manifest.json")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New manifest file required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$sources=@('cmake/compile_definitions/windows.cmake','cmake/dependencies/Pyrowave_Sunshine.cmake',
 'src/audio.h','src/config.cpp','src/config.h','src/rtsp.cpp','src/rtsp.h','src/stream.cpp','src/stream.h','src/thread_safe.h')
$sources+=Get-ChildItem -LiteralPath "$root/src/spatial_transport" -File | ForEach-Object {$_.FullName.Substring($root.Length+1).Replace('\','/')}
foreach($base in @('third-party/moonlight-common-c','spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core/moonlight-common-c')){
 foreach($name in @('AudioStream.c','Connection.c','Limelight.h','Limelight-internal.h','RtspConnection.c','SdpGenerator.c','MatWire.h','MatDumpProfile.h','MatDumpFile.c','MatDumpFile.h','MatDepacketizer.c','MatDepacketizer.h','PyrowaveProtocol.c','PyrowaveProtocol.h','PyrowaveReassembly.c','PyrowaveReassembly.h')){$sources+="$base/src/$name"}
}
foreach($name in @('build-host.ps1','build-client.ps1','session_client.c','verify.py','windres_paths.c','pacing_test.cpp','freeze-proof.ps1','MAT-RTSP-LIVE-RESULT.txt')){$sources+="spatial-object-spike/mat-rtsp-live/$name"}
$hashes=[ordered]@{};foreach($file in $sources|Sort-Object -Unique){$hashes[$file]=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash}
$artifacts=[ordered]@{}
foreach($file in @('build/mat-rtsp-live/sunshine.exe','spatial-object-spike/mat-rtsp-live/client-build/mat-session-client.exe',
 'spatial-object-spike/mat-rtsp-live/run03/proof.json','spatial-object-spike/mat-rtsp-live/run02/proof.json',
 'spatial-object-spike/mat-rtsp-live/common-tests.log','spatial-object-spike/mat-rtsp-live/profile-unit.log',
 'spatial-object-spike/mat-rtsp-live/pacing-test03.log','spatial-object-spike/mat-rtsp-live/android-check.log',
 'spatial-object-spike/mat-rtsp-live/android-objects01/libmat-dump-check.so','build/dlssnr-toolchain/ucrt64/include/winrt/base.h')){
 $artifacts[$file]=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash
}
$pins=[ordered]@{};foreach($path in @('third-party/moonlight-common-c','third-party/pyrowave','third-party/pyrowave/third_party/Granite','spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core/moonlight-common-c')){$pins[$path]=git -C "$root/$path" rev-parse HEAD}
$result=[ordered]@{scope='Full normal Sunshine + actual LiStartConnection, loopback RTSP/RTSPENC and RTP MAT native dump SHA acceptance; NO decode/APK/device test';dependencies=$pins;sources=$hashes;artifacts=$artifacts}
[IO.File]::WriteAllText($Output,($result|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Output 'Frozen actual full RTSP session acceptance evidence'
