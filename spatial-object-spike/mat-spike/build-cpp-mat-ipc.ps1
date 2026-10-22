param([string]$Output="$PSScriptRoot/cpp-mat-ipc-build03")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$compiler='C:/msys64/ucrt64/bin/g++.exe';$env:PATH=(Split-Path -Parent $compiler)+';'+$env:PATH
$deps=(Resolve-Path -LiteralPath "$repo/third-party/build-deps/dist/Windows-AMD64").Path
$boost='C:/msys64/ucrt64/include'
if(-not((Get-Content -LiteralPath "$boost/boost/version.hpp") -match '^#define BOOST_VERSION 108500$')){throw 'Expected installed Boost 1.85'}
$json=(Resolve-Path -LiteralPath "$repo/build/codex/_deps/json-src/single_include").Path
$names=@('cpp_bridge_core.cpp','cpp_bridge_tests.cpp','cpp_bridge_native_v4_tests.cpp','cpp_mat_stream_tests.cpp','cpp_mat_startup_tests.cpp','cpp_mat_ipc_tests.cpp','cpp_mat_ipc_replay.cpp')
$argsList=@('-std=c++20','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-fno-fast-math','-ffp-contract=off','-pthread','-I',"$deps/include",'-I',$json,'-I',$boost)
foreach($name in $names){$argsList+="$PSScriptRoot/$name"}
$argsList+=@('-o',"$out/mat-ipc-replay.exe","$deps/lib/libavutil.a",'-lbcrypt','-lole32','-lgdi32','-luuid','-luser32','-latomic')
& $compiler @argsList
if($LASTEXITCODE){throw 'Build failed'}
$runtime=@();foreach($name in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){$origin=Join-Path (Split-Path -Parent $compiler) $name;Copy-Item -LiteralPath $origin -Destination $out;$runtime+=@{file=$name;sha256=(Get-FileHash -LiteralPath $origin).Hash}}
$test=& "$out/mat-ipc-replay.exe" --selftest;$code=$LASTEXITCODE
$test | Set-Content -LiteralPath "$out/selftest.txt" -Encoding utf8
if($code){throw 'Selftest failed'}
$sources=@();foreach($name in ($names+@('cpp_mat_ipc_protocol.hpp','cpp_mat_ipc_support.hpp','cpp_mat_startup.hpp','cpp_mat_stream.hpp','cpp_mat_stream_fake.hpp','cpp_bridge_core.hpp','cpp_bridge_native_v4.hpp','cpp_bridge_queue.hpp','../resident/quantum_v4_assembler.h','../resident/quantum_v4_protocol.h','../resident/position_cache.h','../resident/protocol.h','build-cpp-mat-ipc.ps1'))){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
[ordered]@{scope='Offline self-owned two-process shared-memory fake encoder only';source_files=$sources;compiler=$compiler
 executable_sha256=(Get-FileHash -LiteralPath "$out/mat-ipc-replay.exe").Hash;runtime=$runtime
 libavutil_sha256=(Get-FileHash -LiteralPath "$deps/lib/libavutil.a").Hash;boost_version='1.85.0'
 native_encoder_called=$false;admin_required=$false;production_default_changed=$false
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
$test
