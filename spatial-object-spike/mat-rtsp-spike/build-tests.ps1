param([Parameter(Mandatory=$true)][string]$Build)
$ErrorActionPreference='Stop';$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/');$out=(Resolve-Path $Build).Path.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$common="$root/spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core/moonlight-common-c"
& gcc.exe -O2 -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "-I$common/nanors" "$PSScriptRoot/audio_client.c" "$out/android-common/libmoonlight-common-c.a" "$out/android-common/enet/libenet.a" -static -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-audio-client.exe"
if($LASTEXITCODE){throw 'Actual AudioStream frontend build failed'}
$existing="$root/build/dlssnr-host";$commands=Get-Content -Raw "$existing/compile_commands.json"|ConvertFrom-Json
$entry=$commands|Where-Object {$_.file.Replace('\','/') -eq "$root/src/main.cpp" -and $_.command -match 'sunshine.dir'}|Select-Object -First 1
$command=$entry.command -replace ' -o \S+ '," -o `"$out/unit.obj`" "
$command=$command -replace ' -c .*$'," -c `"$PSScriptRoot/unit.cpp`""
$command=$command -replace ' -O3 ',' -O1 '
$argsText=$command -replace '^.*?g\+\+\.exe\s+',''
[IO.File]::WriteAllText("$out/unit.rsp",$argsText,[Text.UTF8Encoding]::new($false))
Push-Location $existing
try{& g++.exe "@$out/unit.rsp";if($LASTEXITCODE){throw 'Unit compile failed'}}finally{Pop-Location}
& g++.exe "$out/unit.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatDumpFile.c.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatReceiver.c.obj" "$out/host-common/CMakeFiles/moonlight-common-c.dir/src/MatDepacketizer.c.obj" -lcrypto -lws2_32 -lcrypt32 -lbcrypt -o "$out/mat-profile-unit.exe"
if($LASTEXITCODE){throw 'Unit link failed'}
foreach($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll','libcrypto-3-x64.dll','zlib1.dll')){Copy-Item -LiteralPath "C:/msys64/ucrt64/bin/$dll" -Destination $out}
