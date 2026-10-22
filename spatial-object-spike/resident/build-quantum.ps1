param([string]$Output="$PSScriptRoot/quantum-build02")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new; do not overwrite deployed probes'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compilerRoot='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdkRoot='C:/Program Files (x86)/Windows Kits/10'
$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compilerRoot/include;$sdkRoot/Include/$sdkVersion/ucrt;$sdkRoot/Include/$sdkVersion/shared;$sdkRoot/Include/$sdkVersion/um;$sdkRoot/Include/$sdkVersion/winrt"
$env:LIB="$compilerRoot/lib/x64;$sdkRoot/Lib/$sdkVersion/ucrt/x64;$sdkRoot/Lib/$sdkVersion/um/x64"
Push-Location $out
try {
 $vendor="$PSScriptRoot/vendor/minhook-1.3.4/src"
 & cl.exe /nologo /O2 /W3 /MT /c "$vendor/buffer.c" "$vendor/hook.c" "$vendor/trampoline.c" "$vendor/hde/hde64.c"
 if($LASTEXITCODE){throw 'MinHook compile failed'}
 $objects=@('buffer.obj','hook.obj','trampoline.obj','hde64.obj')
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /LD "$PSScriptRoot/quantum_tap.cpp" @objects /Fe:engine_tap_quantum_v1.dll
 if($LASTEXITCODE){throw 'Quantum tap compile failed'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/quantum_tap_selftest.cpp" @objects /Fe:quantum-tap-selftest.exe
 if($LASTEXITCODE){throw 'Own-process tap selftest compile failed'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/quantum_assembler_test.cpp" /Fe:quantum-assembler-test.exe
 if($LASTEXITCODE){throw 'Assembler test compile failed'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/collector.cpp" /Fe:quantum-collector.exe /link advapi32.lib psapi.lib
 if($LASTEXITCODE){throw 'Collector compile failed'}
 $repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /I "$repo/build/codex/_deps/json-src/single_include" "$PSScriptRoot/quantum_verify.cpp" /Fe:quantum-verify.exe /link bcrypt.lib
 if($LASTEXITCODE){throw 'Read-only quantum verifier compile failed'}
 # Compile the legacy default separately: conditional v3 additions must not break it.
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc /LD "$PSScriptRoot/stateful_tap.cpp" @objects /Fe:stateful-regression.dll
 if($LASTEXITCODE){throw 'Existing stateful tap compile regression'}
 & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/position_cache_test.cpp" /Fe:position-cache-test.exe
 if($LASTEXITCODE){throw 'Cache regression compile failed'}
 foreach($test in @('quantum-tap-selftest','quantum-assembler-test','position-cache-test')) {
  $result=& "./$test.exe"
  $code=$LASTEXITCODE
  $result | Set-Content -LiteralPath "$out/$test.txt" -Encoding utf8
  if($code){throw "$test failed"}
  $result
 }
 $sources=@();foreach($name in @('protocol.h','dolby_tap.cpp','stateful_tap.cpp','quantum_tap.cpp','quantum_tap_selftest.cpp','quantum_assembler.h','quantum_assembler_test.cpp','quantum_verify.cpp','collector.cpp','build-quantum.ps1')) {
  $sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}
 }
 [ordered]@{scope='Build and own-process selftests only. No deployment, elevation, process access or hooks installed.';source_files=$sources
  dll_sha256=(Get-FileHash -LiteralPath "$out/engine_tap_quantum_v1.dll").Hash
  collector_sha256=(Get-FileHash -LiteralPath "$out/quantum-collector.exe").Hash
  live_boundary_verified=$false;mat_encoded=$false
 } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
} finally {Pop-Location}
