param([string]$Output="$PSScriptRoot/cpp-native-v4-build02")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$compiler='C:/msys64/ucrt64/bin/g++.exe';$env:PATH=(Split-Path -Parent $compiler)+';'+$env:PATH
$deps=(Resolve-Path -LiteralPath "$repo/third-party/build-deps/dist/Windows-AMD64").Path
$boost='C:/msys64/ucrt64/include'
# The cached modular 1.92 checkout has no root boost/version.hpp and fell back
# to the system's 1.85 version header. Use the existing complete UCRT 1.85 set,
# without installing anything or modifying the production dependency checkout.
if(-not((Get-Content -LiteralPath "$boost/boost/version.hpp") -match '^#define BOOST_VERSION 108500$')){throw 'Expected existing complete Boost 1.85 headers'}
$json=(Resolve-Path -LiteralPath "$repo/build/codex/_deps/json-src/single_include").Path
$argsList=@('-std=c++20','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-fno-fast-math','-ffp-contract=off','-pthread','-I',"$deps/include",'-I',$json,'-I',$boost)
$argsList+=@("$PSScriptRoot/cpp_bridge_core.cpp","$PSScriptRoot/cpp_bridge_tests.cpp","$PSScriptRoot/cpp_bridge_native_v4_tests.cpp","$PSScriptRoot/cpp_bridge_native_v4_replay.cpp",'-o',"$out/native-v4-replay.exe","$deps/lib/libavutil.a",'-lbcrypt','-lole32','-lgdi32','-luuid','-luser32','-latomic')
& $compiler @argsList
if($LASTEXITCODE){throw 'Build failed'}
$runtime=@();foreach($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){$origin=Join-Path (Split-Path -Parent $compiler) $dll;Copy-Item -LiteralPath $origin -Destination $out;$runtime+=@{file=$dll;sha256=(Get-FileHash -LiteralPath $origin).Hash}}
$selftest=& "$out/native-v4-replay.exe" --selftest;$code=$LASTEXITCODE
$selftest | Set-Content -LiteralPath "$out/selftest.txt" -Encoding utf8
if($code){throw 'Selftest failed'}
$sources=@();foreach($name in @('cpp_bridge_native_v4.hpp','cpp_bridge_native_v4_tests.cpp','cpp_bridge_native_v4_replay.cpp','build-cpp-native-v4.ps1','cpp_bridge_core.hpp','cpp_bridge_core.cpp','cpp_bridge_tests.cpp','cpp_bridge_queue.hpp','../resident/quantum_v4_assembler.h','../resident/quantum_v4_protocol.h','../resident/position_cache.h','../resident/protocol.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
[ordered]@{scope='Offline worker only. No admin, audio, process access, codec, RTP or production changes.';compiler=$compiler;source_files=$sources;runtime=$runtime
 executable_sha256=(Get-FileHash -LiteralPath "$out/native-v4-replay.exe").Hash;libavutil_sha256=(Get-FileHash -LiteralPath "$deps/lib/libavutil.a").Hash
 boost_include_root=$boost;boost_version='1.85.0'
 boost_spsc_sha256=(Get-FileHash -LiteralPath "$boost/boost/lockfree/spsc_queue.hpp").Hash
 boost_version_header_sha256=(Get-FileHash -LiteralPath "$boost/boost/version.hpp").Hash
 gain_policy='native combined F32; legacy Bridge source stays unchanged';fp_options='-fno-fast-math -ffp-contract=off';native_encoder_called=$false
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
$selftest
