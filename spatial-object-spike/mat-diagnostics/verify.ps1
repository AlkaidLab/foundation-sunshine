param([Parameter(Mandatory=$true)][string]$Output,[Parameter(Mandatory=$true)][string]$CommonBuild)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh diagnostic output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
New-Item -ItemType Directory -Path "$out/cases"|Out-Null
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$common="$root/third-party/moonlight-common-c";$lib=(Resolve-Path -LiteralPath $CommonBuild).Path
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$objects=@("$lib/CMakeFiles/moonlight-common-c.dir/src/MatPlayback.c.obj","$lib/CMakeFiles/moonlight-common-c.dir/src/MatReceiver.c.obj","$lib/CMakeFiles/moonlight-common-c.dir/src/MatDepacketizer.c.obj")
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation "$PSScriptRoot/test.c" "$PSScriptRoot/MatDumpFile.c" @objects -lcrypto -o "$out/diagnostic-test.exe"
if($LASTEXITCODE){throw 'Diagnostic writer test build failed'}
& "$out/diagnostic-test.exe" "$out/cases" *> "$out/test.log"
if($LASTEXITCODE){Get-Content "$out/test.log";throw 'Diagnostic writer/recording regression failed'}
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror "$PSScriptRoot/replay.c" "$lib/CMakeFiles/moonlight-common-c.dir/src/MatReceiver.c.obj" "$lib/CMakeFiles/moonlight-common-c.dir/src/MatDepacketizer.c.obj" -o "$out/mat-recording-check.exe"
if($LASTEXITCODE){throw 'Offline recording tool build failed'}
& "$out/mat-recording-check.exe" "$out/cases/case0.raw" *> "$out/replay.log"
if($LASTEXITCODE){throw 'Tool-owned generated recording replay failed'}
[IO.File]::WriteAllBytes("$out/truncated.raw",[byte[]](0..7))
& "$out/mat-recording-check.exe" "$out/truncated.raw" *> "$out/truncated.log"
if($LASTEXITCODE -ne 1){throw 'Truncated recording was accepted'}
# Both actual AudioStream dump frontends must compile using the new callbacks.
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/../mat-rtsp-spike/audio_client.c" "$PSScriptRoot/MatDumpFile.c" "$lib/libmoonlight-common-c.a" "$lib/enet/libenet.a" -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-audio-client.exe"
if($LASTEXITCODE){throw 'Tool-owned AudioStream dump frontend build failed'}
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/../mat-rtsp-live/session_client.c" "$PSScriptRoot/MatDumpFile.c" "$lib/libmoonlight-common-c.a" "$lib/enet/libenet.a" -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-session-client.exe"
if($LASTEXITCODE){throw 'Tool-owned RTSP dump frontend build failed'}
@{result='PASS';dumpCases=7;recordingCases=7;replayConsumer='mock';generatedBytesOnly=$true;nativeCapture=$false;hardwareOutput=$false}|ConvertTo-Json|Set-Content "$out/proof.json"
Get-Content "$out/test.log","$out/replay.log"
exit 0
