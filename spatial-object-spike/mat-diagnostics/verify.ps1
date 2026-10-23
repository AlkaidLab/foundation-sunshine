param([Parameter(Mandatory=$true)][string]$Output,[Parameter(Mandatory=$true)][string]$CommonBuild,[string]$Python='python.exe')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh diagnostic output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
New-Item -ItemType Directory -Path "$out/cases"|Out-Null
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$common="$root/third-party/moonlight-common-c";$lib=(Resolve-Path -LiteralPath $CommonBuild).Path
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$objects=@("$PSScriptRoot/MatPlayback.c","$lib/CMakeFiles/moonlight-common-c.dir/src/MatReceiver.c.obj","$lib/CMakeFiles/moonlight-common-c.dir/src/MatDepacketizer.c.obj")
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
# Live-only mock frontend: production receive/decoder threads, no file sink or device.
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/live_client.c" "$lib/libmoonlight-common-c.a" "$lib/enet/libenet.a" -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-audio-client.exe"
if($LASTEXITCODE){throw 'Live mock AudioStream frontend build failed'}
& $Python "$PSScriptRoot/watchdog_test.py" --client "$out/mat-audio-client.exe" --output "$out/watchdog" *> "$out/watchdog.log"
if($LASTEXITCODE){throw 'Real receive-thread EOS/inactivity regression failed'}
@{result='PASS';dumpCases=7;recordingCases=7;threadedWatchdogCases=3;replayConsumer='mock';generatedBytesOnly=$true;nativeCapture=$false;hardwareOutput=$false}|ConvertTo-Json|Set-Content "$out/proof.json"
Get-Content "$out/test.log","$out/replay.log"
exit 0
