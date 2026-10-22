param([Parameter(Mandatory=$true)][string]$Output,[switch]$SkipHostBuild,[int]$Port=52210,
 [string]$Python='C:/Python311/python.exe')
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'New verification directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
Push-Location $root
try {
 & "$PSScriptRoot/build.ps1" -Output "$out/client"
 & "$root/spatial-object-spike/mat-android-offline/verify.ps1" -Output "$out/regression"
 & gcc.exe -O2 '-Ithird-party/moonlight-common-c/src' "$PSScriptRoot/playback_unit.c" third-party/moonlight-common-c/src/MatPlayback.c third-party/moonlight-common-c/src/MatReceiver.c third-party/moonlight-common-c/src/MatDepacketizer.c -lcrypto -o "$out/playback-unit.exe"
 if($LASTEXITCODE){throw 'Playback lifecycle unit build failed'}
 & "$out/playback-unit.exe" "$root/spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw" | Tee-Object -FilePath "$out/playback-unit.log"
 if($LASTEXITCODE){throw 'Playback lifecycle unit failed'}
 & $Python "$PSScriptRoot/negotiation_test.py" --client "$out/client/client-command.json" --output "$out/negative"
 if($LASTEXITCODE){throw 'Actual client negative negotiation failed'}
 if(!$SkipHostBuild){& "$root/spatial-object-spike/mat-rtsp-live/build-host.ps1"}
 & "$root/spatial-object-spike/mat-rtsp-live/build-client.ps1"
 & $Python "$root/spatial-object-spike/mat-rtsp-live/verify.py" --output "$out/playback" --port $Port --playback-client "$out/client/client-command.json"
 if($LASTEXITCODE){throw 'Full production playback session failed'}
 & $Python "$root/spatial-object-spike/mat-rtsp-live/verify.py" --output "$out/dump" --port $Port
 if($LASTEXITCODE){throw 'Original dump session regression failed'}
 & "$PSScriptRoot/freeze-proof.ps1" -Playback "$out/playback" -Dump "$out/dump" -Negative "$out/negative" -Regression "$out/regression" -NativeBuild "$out/client" -Output "$out/proof.json"
} finally {Pop-Location}
