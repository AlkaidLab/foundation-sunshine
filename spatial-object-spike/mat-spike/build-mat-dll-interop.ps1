param([string]$Output="$PSScriptRoot/mat-dll-interop-build01",[string]$ReceiverBuild="$PSScriptRoot/mat-ipc-receiver-build06")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$receiver=Get-Content -LiteralPath "$ReceiverBuild/build.json" -Raw | ConvertFrom-Json
foreach($entry in $receiver.source_files){if((Get-FileHash -LiteralPath "$PSScriptRoot/$($entry.file)").Hash -ne $entry.sha256){throw 'Frozen receiver source changed'}}
$dll=(Resolve-Path -LiteralPath "$ReceiverBuild/mat_ipc_receiver.dll").Path
if((Get-FileHash -LiteralPath $dll).Hash -ne $receiver.dll_sha256){throw 'Frozen DLL hash changed'}
$json=(Resolve-Path -LiteralPath "$repo/build/codex/_deps/json-src/single_include").Path
$gcc='C:/msys64/ucrt64/bin/g++.exe';$env:PATH=(Split-Path -Parent $gcc)+';'+$env:PATH
$deps=(Resolve-Path -LiteralPath "$repo/third-party/build-deps/dist/Windows-AMD64").Path
$argsList=@('-std=c++20','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-fno-fast-math','-ffp-contract=off','-pthread','-I',"$deps/include",'-I',$json,'-I','C:/msys64/ucrt64/include',"$PSScriptRoot/cpp_bridge_core.cpp","$PSScriptRoot/cpp_mat_ipc_tests.cpp","$PSScriptRoot/mat_dll_ipc_producer.cpp",'-o',"$out/mat-dll-producer.exe","$deps/lib/libavutil.a",'-lbcrypt','-lole32','-lgdi32','-luuid','-luser32','-latomic')
& $gcc @argsList
if($LASTEXITCODE){throw 'GCC producer build failed'}
$runtime=@();foreach($name in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){$origin=Join-Path (Split-Path -Parent $gcc) $name;Copy-Item -LiteralPath $origin -Destination $out;$runtime+=@{file=$name;sha256=(Get-FileHash -LiteralPath $origin).Hash}}
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519';$sdk='C:/Program Files (x86)/Windows Kits/10';$sdkVersion='10.0.22621.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
Push-Location $out
try{& cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /I $json "$PSScriptRoot/mat_dll_ipc_host.cpp" /Fe:mat-dll-host.exe;if($LASTEXITCODE){throw 'MSVC child host build failed'}}finally{Pop-Location}
$test=& "$out/mat-dll-producer.exe" --selftest;$code=$LASTEXITCODE
$test | Set-Content -LiteralPath "$out/protocol-selftest.txt" -Encoding utf8
if($code){throw 'Frozen protocol regressions failed'}
$sources=@();foreach($name in @('mat_dll_ipc_producer.cpp','mat_dll_ipc_host.cpp','build-mat-dll-interop.ps1','cpp_mat_ipc_protocol.hpp','cpp_mat_ipc_support.hpp','cpp_mat_ipc_tests.cpp','cpp_bridge_core.hpp','cpp_bridge_core.cpp','cpp_bridge_native_v4.hpp','cpp_bridge_queue.hpp','cpp_mat_stream.hpp','cpp_mat_stream_fake.hpp','mat_ipc_receiver_api.h','../resident/quantum_v4_assembler.h','../resident/quantum_v4_protocol.h','../resident/position_cache.h','../resident/protocol.h','../resident/encoded_protocol.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
[ordered]@{scope='Own normal GCC producer -> own normal MSVC child -> frozen DLL FAKE export only. No native entry, elevation, deployment, audio or foreign process attachment.';source_files=$sources
 producer_sha256=(Get-FileHash -LiteralPath "$out/mat-dll-producer.exe").Hash;host_sha256=(Get-FileHash -LiteralPath "$out/mat-dll-host.exe").Hash
 receiver_dll=$dll;receiver_dll_sha256=$receiver.dll_sha256;runtime=$runtime;receiver_build_manifest_sha256=(Get-FileHash -LiteralPath "$ReceiverBuild/build.json").Hash
 native_encoder_called=$false;vendor_loaded=$false;audiodg_accessed=$false;production_default_changed=$false
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
$test
