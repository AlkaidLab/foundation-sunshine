param([string]$Output="$PSScriptRoot/mat-native-v4-build01",[string]$InputDirectory="$PSScriptRoot/mat-native-v4-input01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$inputFile=(Resolve-Path -LiteralPath "$InputDirectory/input.bin").Path
$inputInfo=Get-Content -LiteralPath "$InputDirectory/manifest.json" -Raw | ConvertFrom-Json
if((Get-FileHash -LiteralPath $inputFile).Hash -ne $inputInfo.input_sha256){throw 'Prepared input hash mismatch'}
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdk='C:/Program Files (x86)/Windows Kits/10';$version='10.0.22621.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um;$sdk/Include/$version/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
$json=(Resolve-Path -LiteralPath "$PSScriptRoot/../../build/codex/_deps/json-src/single_include").Path
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$PSScriptRoot/mat_replay_v4_dll.cpp" /Fe:mat_replay_v4.dll /link runtimeobject.lib ole32.lib
 if($LASTEXITCODE){throw 'Replay DLL compile failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /I $json "$PSScriptRoot/mat_replay_v4_probe.cpp" /Fe:mat-replay-v4-probe.exe /link advapi32.lib psapi.lib
 if($LASTEXITCODE){throw 'Replay probe compile failed'}
 & ./mat-replay-v4-probe.exe --selftest "$out/mat_replay_v4.dll" $inputFile "$out/selftest"
 if($LASTEXITCODE){throw 'Replay own-process selftest failed'}
 $sources=@();foreach($name in @('mat_replay_v4_protocol.h','mat_replay_v4_input.h','mat_replay_v4_dll.cpp','mat_replay_v4_probe.cpp','build-mat-native-v4.ps1','mat_lifecycle_protocol.h','../resident/collector.cpp','../resident/protocol.h','../resident/encoded_protocol.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
 [ordered]@{scope='Build + own-process fake interface/input/host-guard tests only. No elevation, deployment, audio activation, process attachment or vendor load.';source_files=$sources
  dll_sha256=(Get-FileHash -LiteralPath "$out/mat_replay_v4.dll").Hash;probe_sha256=(Get-FileHash -LiteralPath "$out/mat-replay-v4-probe.exe").Hash
  input_sha256=$inputInfo.input_sha256;native_encoder_called=$false;production_default_changed=$false
 } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
}finally{Pop-Location}
