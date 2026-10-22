param(
 [string]$Playback="$PSScriptRoot/session-final",
 [string]$Dump="$PSScriptRoot/dump-session-final",
 [string]$Negative="$PSScriptRoot/negative03",
 [string]$Regression="$PSScriptRoot/regression-final",
 [string]$NativeBuild="$PSScriptRoot/build03",
 [string]$Output="$PSScriptRoot/final-evidence.json"
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New evidence filename required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$play=Get-Content -Raw "$Playback/proof.json"|ConvertFrom-Json
$dumpProof=Get-Content -Raw "$Dump/proof.json"|ConvertFrom-Json
$negativeProof=Get-Content -Raw "$Negative/proof.json"|ConvertFrom-Json
$regressionProof=Get-Content -Raw "$Regression/proof.json"|ConvertFrom-Json
$expected='79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6'
if($play.results.Count -ne 3 -or $dumpProof.results.Count -ne 3 -or $negativeProof.results.Count -ne 15 -or $regressionProof.result -ne 'PASS'){throw 'Incomplete acceptance'}
foreach($case in $play.results){if($case.sha256 -ne $expected -or $case.bytes -ne 10936320 -or !$case.status.completed -or !$case.playbackFakeHardware){throw 'Playback acceptance mismatch'}}
foreach($case in $dumpProof.results){if($case.sha256 -ne $expected -or !$case.status.published -or $case.playbackFakeHardware){throw 'Dump regression mismatch'}}
foreach($case in $negativeProof.results){if(!$case.rejected -or $case.carrierBytesDelivered -ne 0){throw 'Negative acceptance mismatch'}}
$sources=@('src/audio.h','src/rtsp.cpp','src/rtsp.h','src/stream.cpp','src/spatial_transport/mat_dump_profile.h',
 'cmake/compile_definitions/windows.cmake','spatial-object-spike/mat-rtsp-live/build-host.ps1',
 'spatial-object-spike/mat-rtsp-live/verify.py','spatial-object-spike/mat-rtsp-live/windres_paths.c',
 'spatial-object-spike/mat-rtsp-spike/unit.cpp','spatial-object-spike/mat-rtsp-spike/check-android.ps1',
 'spatial-object-spike/mat-android-offline/verify.ps1')
$shared=@('Limelight.h','Limelight-internal.h','Connection.c','RtspConnection.c','RtspParser.c','Rtsp.h',
 'SdpGenerator.c','AudioStream.c','MatPlayback.h','MatPlayback.c','MatPlaybackProfile.h',
 'MatReceiver.h','MatReceiver.c','MatDumpFile.c','MatDumpProfile.h','MatDepacketizer.c','MatDepacketizer.h','MatWire.h')
$android='spatial-object-spike/moonlight-vplus/app'
$sources+=@('third-party/moonlight-common-c/CMakeLists.txt','third-party/moonlight-common-c/tests/RtspParserMatTest.c')
foreach($name in $shared){
 $hostFile="third-party/moonlight-common-c/src/$name"
 $clientFile="$android/src/main/jni/moonlight-core/moonlight-common-c/src/$name"
 $hostText=(Get-Content -Raw "$root/$hostFile").Replace("`r`n","`n")
 $clientText=(Get-Content -Raw "$root/$clientFile").Replace("`r`n","`n")
 # Legacy common-c files contain pre-existing comment differences. Preserve them;
 # compare the shared MAT modules exactly, and fingerprint both legacy copies.
 if($name.StartsWith('Mat') -and $hostText -cne $clientText){throw "Shared MAT module mismatch: $name"}
 $sources+=@($hostFile,$clientFile)
}
foreach($name in @('MatCarrierConsumer.java','MatCarrierPlayer.java','MatPlaybackSession.java','MatPassthroughRenderer.kt','MatAudioTrackSink.java','SmartAudioRenderer.kt','MatDumpRenderer.kt')){
 $sources+="$android/src/main/java/com/limelight/binding/audio/$name"
}
$sources+=@("$android/src/main/java/com/limelight/nvstream/jni/MoonBridge.java",
 "$android/src/main/java/com/limelight/preferences/PreferenceConfiguration.kt","$android/src/main/java/com/limelight/Game.kt",
 "$android/src/main/res/values/arrays.xml","$android/src/main/jni/moonlight-core/Android.mk",
 "$android/src/main/jni/moonlight-core/callbacks.c","$android/src/main/jni/moonlight-core/mat_carrier_bridge.c",
 "$android/src/main/jni/moonlight-core/mat_carrier_bridge.h")
foreach($name in @('build.ps1','verify.ps1','freeze-proof.ps1','session_jni.c','MatSessionClient.java','playback_unit.c','negotiation_test.py')){
 $sources+="spatial-object-spike/mat-android-session/$name"
}
$hashes=[ordered]@{}
foreach($name in $sources|Sort-Object -Unique){$hashes[$name]=(Get-FileHash -LiteralPath "$root/$name" -Algorithm SHA256).Hash.ToLower()}
$artifacts=[ordered]@{}
foreach($path in @("$Playback/proof.json","$Dump/proof.json","$Negative/proof.json","$Regression/proof.json",
 "$NativeBuild/moonlight-mat-session.dll","$NativeBuild/client-command.json","$root/build/mat-rtsp-live/sunshine.exe")){
 $artifacts[[IO.Path]::GetFullPath($path)]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
}
foreach($file in Get-ChildItem -LiteralPath "$NativeBuild/classes" -Recurse -Filter '*.class'){
 $artifacts[$file.FullName]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLower()
}
if((Get-FileHash -LiteralPath "$root/build/mat-rtsp-live/sunshine.exe").Hash.ToLower() -ne $play.hostSha256){throw 'Tested host changed'}
if($play.hostSha256 -ne $dumpProof.hostSha256){throw 'Playback and dump used different hosts'}
$repositories=[ordered]@{}
foreach($name in @('host','client','common','pyrowave','granite')){
 $repo=switch($name){
  host{$root} client{"$root/spatial-object-spike/moonlight-vplus"}
  common{"$root/third-party/moonlight-common-c"} pyrowave{"$root/third-party/pyrowave"}
  granite{"$root/third-party/pyrowave/third_party/Granite"}
 }
 $repositories[$name]=git -C $repo rev-parse HEAD
 if($LASTEXITCODE){throw "Cannot identify tested repository: $name"}
}
@{result='PASS';scope='C finite MAT playback: full production Sunshine RTSP/RTP + Android-checkout common-c + production JNI/Java session, fake hardware only';
 playback=$play.results;dump=$dumpProof.results;negative=$negativeProof.results;sourceSha256=$expected;
 androidDeviceTested=$false;fullMoonlightApkBuilt=$false;repositories=$repositories;sourceHashes=$hashes;artifacts=$artifacts}|
 ConvertTo-Json -Depth 9|Out-File -LiteralPath $Output -Encoding utf8
Write-Host "PASS frozen C evidence: $Output"
