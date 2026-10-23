param([Parameter(Mandatory=$true)][string]$Build)
$ErrorActionPreference='Stop';$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/');$out=(Resolve-Path $Build).Path.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$common="$root/third-party/moonlight-common-c"
& gcc.exe -O2 -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "-I$common/nanors" "$PSScriptRoot/audio_client.c" "$PSScriptRoot/../mat-diagnostics/MatDumpFile.c" "$out/host-common/libmoonlight-common-c.a" "$out/host-common/enet/libenet.a" -static -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-audio-client.exe"
if($LASTEXITCODE){throw 'Actual AudioStream frontend build failed'}
# The profile/lifecycle unit needs only repository headers and OpenSSL, not a
# previous full-host compile database with another machine's absolute paths.
& g++.exe -std=c++20 -O1 "-I$root" -c "$PSScriptRoot/unit.cpp" -o "$out/unit.obj"
if($LASTEXITCODE){throw 'Unit compile failed'}
& gcc.exe -O2 -c "$PSScriptRoot/../mat-diagnostics/MatDumpFile.c" -o "$out/dump-tool.obj"
if($LASTEXITCODE){throw 'Diagnostic writer compile failed'}
& g++.exe "$out/unit.obj" "$out/dump-tool.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatPlayback.c.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatReceiver.c.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatDepacketizer.c.obj" -lcrypto -lws2_32 -lcrypt32 -lbcrypt -o "$out/mat-profile-unit.exe"
if($LASTEXITCODE){throw 'Unit link failed'}
foreach($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll','libcrypto-3-x64.dll','zlib1.dll')){Copy-Item -LiteralPath "C:/msys64/ucrt64/bin/$dll" -Destination $out}
