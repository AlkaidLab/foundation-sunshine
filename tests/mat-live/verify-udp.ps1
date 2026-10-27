param([Parameter(Mandatory=$true)][string]$Output,[Parameter(Mandatory=$true)][string]$CommonBuild,[string]$Python='python.exe')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh UDP evidence directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$common="$root/third-party/moonlight-common-c";$lib=(Resolve-Path -LiteralPath $CommonBuild).Path
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/audio_client.c" "$lib/libmoonlight-common-c.a" "$lib/enet/libenet.a" -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-audio-client.exe"
if($LASTEXITCODE){throw 'Live mock AudioStream build failed'}
& $Python "$PSScriptRoot/watchdog_test.py" --client "$out/mat-audio-client.exe" --output "$out/watchdog" *> "$out/watchdog.log"
if($LASTEXITCODE){throw 'Actual UDP receive/decoder thread regression failed'}
Get-Content "$out/watchdog.log"
