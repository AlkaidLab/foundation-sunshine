param([string]$Output="$PSScriptRoot/spatial-broker-build01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path;$source=Join-Path $repo 'src/spatial_audio'
$json=(Resolve-Path -LiteralPath "$repo/build/codex/_deps/json-src/single_include").Path
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519';$sdk='C:/Program Files (x86)/Windows Kits/10';$sdkVersion='10.0.22621.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$source/capture_reader_msvc.cpp" /Fe:spatial_capture_adapter.dll
 if($LASTEXITCODE){throw 'Capture adapter build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$source/mat_stream_receiver.cpp" /Fe:mat_stream_receiver.dll /link runtimeobject.lib ole32.lib bcrypt.lib
 if($LASTEXITCODE){throw 'Streaming receiver build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /MT /EHsc /I $json "$source/capture_fixture_msvc.cpp" /Fe:spatial-capture-fixture.exe
 if($LASTEXITCODE){throw 'Own ring fixture build failed'}
}finally{Pop-Location}
$gcc='C:/msys64/ucrt64/bin/g++.exe';$env:PATH=(Split-Path -Parent $gcc)+';'+$env:PATH
$deps=(Resolve-Path -LiteralPath "$repo/third-party/build-deps/dist/Windows-AMD64").Path
$compileArgs=@('-std=c++20','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-fno-fast-math','-ffp-contract=off','-pthread','-I',"$deps/include",'-I',$json,'-I','C:/msys64/ucrt64/include',"$PSScriptRoot/cpp_bridge_core.cpp","$source/broker_tool.cpp",'-o',"$out/spatial-audio-broker.exe","$deps/lib/libavutil.a",'-lbcrypt','-lole32','-lgdi32','-luuid','-luser32','-latomic')
& $gcc @compileArgs
if($LASTEXITCODE){throw 'GCC broker build failed'}
$runtime=@();foreach($name in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){$origin=Join-Path (Split-Path -Parent $gcc) $name;Copy-Item -LiteralPath $origin -Destination $out;$runtime+=@{file=$name;sha256=(Get-FileHash -LiteralPath $origin).Hash}}
& "$out/spatial-audio-broker.exe" --selftest "$out/selftest"
if($LASTEXITCODE){throw 'Broker selftest failed'}
$sources=@();foreach($entry in (Get-ChildItem -LiteralPath $source -File)){$sources+=@{file="../../src/spatial_audio/$($entry.Name)";sha256=(Get-FileHash -LiteralPath $entry.FullName).Hash}}
foreach($name in @('build-spatial-broker.ps1','cpp_bridge_core.cpp','cpp_bridge_core.hpp','cpp_bridge_native_v4.hpp','cpp_mat_stream.hpp','cpp_mat_ipc_protocol.hpp','cpp_mat_ipc_support.hpp','../resident/quantum_v4_assembler.h','../resident/quantum_v4_protocol.h','../resident/position_cache.h','../resident/protocol.h','../resident/encoded_protocol.h','mat_ipc_receiver_api.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
$binaries=@();foreach($name in @('spatial_capture_adapter.dll','mat_stream_receiver.dll','spatial-capture-fixture.exe','spatial-audio-broker.exe')){$binaries+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$out/$name").Hash}}
[ordered]@{scope='M1/M2/M3 opt-in modules. Build and own-process fake tests only. No deployment/elevation/audiodg/RTP/default-path change.';source_files=$sources;binaries=$binaries;runtime=$runtime;native_encoder_called=$false;audiodg_accessed=$false} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
