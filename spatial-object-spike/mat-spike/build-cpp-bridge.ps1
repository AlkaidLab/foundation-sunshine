param([string]$Output="$PSScriptRoot/cpp-bridge-build05")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$repo=(Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
$compiler='C:/msys64/ucrt64/bin/g++.exe'
if(-not(Test-Path -LiteralPath $compiler)){throw 'Expected existing UCRT64 compiler, do not install or mix ABI silently'}
$env:PATH=(Split-Path -Parent $compiler)+';'+$env:PATH
$deps=(Resolve-Path -LiteralPath "$repo/third-party/build-deps/dist/Windows-AMD64").Path
$boost=(Resolve-Path -LiteralPath "$repo/build/codex-902/_deps/ex-boost-populate1234/boost-1.92.0").Path
$json=(Resolve-Path -LiteralPath "$repo/build/codex/_deps/json-src/single_include").Path
$argsList=@('-std=c++20','-O2','-Wall','-Wextra','-Wno-misleading-indentation','-fno-fast-math','-ffp-contract=off','-pthread','-I',"$deps/include",'-I',$json)
foreach($component in @('assert','config','core','lockfree','parameter','predef','type_traits','mp11','throw_exception','static_assert','concept_check','utility','mpl','integer')){
 $include=Join-Path $boost "libs/$component/include"
 if(Test-Path -LiteralPath $include){$argsList+=@('-I',$include)}
}
$argsList+=@("$PSScriptRoot/cpp_bridge_core.cpp","$PSScriptRoot/cpp_bridge_tests.cpp","$PSScriptRoot/cpp_bridge_replay.cpp",'-o',"$out/cpp-bridge-replay.exe","$deps/lib/libavutil.a",'-lbcrypt','-lole32','-lgdi32','-luuid','-luser32','-latomic')
& $compiler @argsList
if($LASTEXITCODE){throw 'C++ bridge compile/link failed; inspect before changing dependency versions'}
# This installed GCC 16.2 static EH/TLS runtime crashes even for an isolated
# throw/catch. Keep the same UCRT compiler/FFmpeg ABI, ship its existing runtime
# DLLs beside this diagnostic executable, and exercise exceptions in selftests.
$runtime=@()
foreach($dll in @('libstdc++-6.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){
 $origin=Join-Path (Split-Path -Parent $compiler) $dll
 Copy-Item -LiteralPath $origin -Destination $out
 $runtime+=@{file=$dll;source=$origin;sha256=(Get-FileHash -LiteralPath $origin).Hash}
}
$selftest=& "$out/cpp-bridge-replay.exe" --selftest
$selftestExit=$LASTEXITCODE
$selftest | Set-Content -LiteralPath "$out/selftest.json" -Encoding utf8
if($selftestExit){throw 'C++ bridge selftests failed'}
$sources=@()
foreach($name in @('cpp_bridge_core.hpp','cpp_bridge_core.cpp','cpp_bridge_queue.hpp','cpp_bridge_tests.cpp','cpp_bridge_replay.cpp','build-cpp-bridge.ps1','../resident/position_cache.h','../resident/protocol.h')){
 $path=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot $name)).Path
 $sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath $path).Hash}
}
[ordered]@{compiler=$compiler;libavutil="$deps/lib/libavutil.a";libavutil_sha256=(Get-FileHash -LiteralPath "$deps/lib/libavutil.a").Hash
 runtime=$runtime
 source_files=$sources
 boost=$boost;boost_spsc_sha256=(Get-FileHash -LiteralPath "$boost/libs/lockfree/include/boost/lockfree/spsc_queue.hpp").Hash
 binary_sha256=(Get-FileHash -LiteralPath "$out/cpp-bridge-replay.exe").Hash
 floating_point='no fast math; FP contraction off; separate F32 volume and callGain multiplications';scope='Offline only. No admin/game/vendor encoder or production dependency changes.'
} | ConvertTo-Json | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
$selftest
