param([string]$Output="$PSScriptRoot/quantum-v4-build06")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new; never overwrite the previous deployed v3 DLL'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compilerRoot='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdkRoot='C:/Program Files (x86)/Windows Kits/10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compilerRoot/include;$sdkRoot/Include/$sdkVersion/ucrt;$sdkRoot/Include/$sdkVersion/shared;$sdkRoot/Include/$sdkVersion/um;$sdkRoot/Include/$sdkVersion/winrt"
$env:LIB="$compilerRoot/lib/x64;$sdkRoot/Lib/$sdkVersion/ucrt/x64;$sdkRoot/Lib/$sdkVersion/um/x64"
Push-Location $out
try {
 $vendor="$PSScriptRoot/vendor/minhook-1.3.4/src"
 & cl.exe /nologo /O2 /W3 /MT /c "$vendor/buffer.c" "$vendor/hook.c" "$vendor/trampoline.c" "$vendor/hde/hde64.c"
 if($LASTEXITCODE){throw 'MinHook compile failed'}
 $objects=@('buffer.obj','hook.obj','trampoline.obj','hde64.obj')
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /LD "$PSScriptRoot/quantum_v4_tap.cpp" "$PSScriptRoot/quantum_v4_recorder.cpp" @objects /Fe:engine_tap_quantum_v4.dll
 if($LASTEXITCODE){throw 'v4 DLL compile failed'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/quantum_v4_selftest.cpp" "$PSScriptRoot/quantum_v4_recorder.cpp" @objects /Fe:quantum-v4-selftest.exe
 if($LASTEXITCODE){throw 'v4 own-process test compile failed'}
 $repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
 $json="$repo/build/codex/_deps/json-src/single_include"
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /I $json "$PSScriptRoot/quantum_v4_collector.cpp" /Fe:quantum-v4-collector.exe /link advapi32.lib psapi.lib
 if($LASTEXITCODE){throw 'v4 collector compile failed'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /I $json "$PSScriptRoot/quantum_v4_verify.cpp" /Fe:quantum-v4-verify.exe /link bcrypt.lib
 if($LASTEXITCODE){throw 'v4 read-only verifier compile failed'}
 $result=& ./quantum-v4-selftest.exe --fixture-output "$out/fixture"
 $code=$LASTEXITCODE
 $result | Set-Content -LiteralPath "$out/selftest.txt" -Encoding utf8
 if($code){throw 'v4 tests failed'}
 $result
 $gcc='C:/msys64/ucrt64/bin/g++.exe';$env:PATH=(Split-Path -Parent $gcc)+';'+$env:PATH
 $deps="$repo/third-party/build-deps/dist/Windows-AMD64"
 & $gcc -std=c++20 -O2 -Wall -Wextra -Wno-misleading-indentation -fno-fast-math -ffp-contract=off -pthread -I "$deps/include" "$PSScriptRoot/../mat-spike/quantum_v4_bridge_test.cpp" "$PSScriptRoot/../mat-spike/cpp_bridge_core.cpp" -o "$out/quantum-v4-bridge-test.exe" "$deps/lib/libavutil.a" -lbcrypt -lole32 -lgdi32 -luuid -luser32 -latomic
 if($LASTEXITCODE){throw 'Existing FFmpeg bridge integration compile failed'}
 $runtimes=@()
 foreach($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){$origin=Join-Path (Split-Path -Parent $gcc) $dll;Copy-Item -LiteralPath $origin -Destination $out;$runtimes+=@{file=$dll;sha256=(Get-FileHash -LiteralPath $origin).Hash}}
 $bridge=& ./quantum-v4-bridge-test.exe "$out/fixture/events.bin";$code=$LASTEXITCODE
 $bridge | Set-Content -LiteralPath "$out/bridge-selftest.json" -Encoding utf8
 if($code){throw 'Existing FFmpeg bridge integration failed'}
 $bridge
 $sources=@();foreach($name in @('protocol.h','position_cache.h','collector.cpp','quantum_v4_collector.cpp','quantum_v4_verify.cpp','quantum_v4_protocol.h','quantum_v4_recorder.h','quantum_v4_recorder.cpp','quantum_v4_tap.cpp','quantum_v4_assembler.h','quantum_v4_selftest.cpp','build-quantum-v4.ps1','../mat-spike/quantum_v4_bridge_test.cpp','../mat-spike/cpp_bridge_core.cpp','../mat-spike/cpp_bridge_core.hpp')){
  $sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}
 }
 [ordered]@{scope='Offline build + own-process fake originals. No elevation/deployment/audio/process access.';source_files=$sources
  runtime=$runtimes;libavutil_sha256=(Get-FileHash -LiteralPath "$deps/lib/libavutil.a").Hash
  dll_sha256=(Get-FileHash -LiteralPath "$out/engine_tap_quantum_v4.dll").Hash;collector_sha256=(Get-FileHash -LiteralPath "$out/quantum-v4-collector.exe").Hash
  verifier_sha256=(Get-FileHash -LiteralPath "$out/quantum-v4-verify.exe").Hash;bridge_test_sha256=(Get-FileHash -LiteralPath "$out/quantum-v4-bridge-test.exe").Hash
  native_verified=$false;production_default_changed=$false
 } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
} finally {Pop-Location}
