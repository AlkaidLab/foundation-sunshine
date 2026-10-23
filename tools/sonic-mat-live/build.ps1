param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh Sonic/MAT build output required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path;$out=(New-Item -ItemType Directory -Path $Output).FullName
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207';$sdk='C:/Program Files (x86)/Windows Kits/10';$v='10.0.26100.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$v/ucrt;$sdk/Include/$v/shared;$sdk/Include/$v/um;$sdk/Include/$v/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$v/ucrt/x64;$sdk/Lib/$v/um/x64"
$minhook="$root/build/minhook-v1.3.4";if((git -C $minhook rev-parse HEAD) -ne 'c3fcafdc10146beb5919319d0683e44e3c30d537'){throw 'Pinned MinHook required'}
Push-Location $out
try {
 & cl.exe /nologo /std:c++20 /O2 /W4 /WX /MT /EHsc "$PSScriptRoot/capture_options_test.cpp" /Fe:capture-options-test.exe
 if($LASTEXITCODE){throw 'Capture options test build failed'}
 & ./capture-options-test.exe *> capture-options-test.log
 if($LASTEXITCODE){throw 'Capture options tests failed'}
 & cl.exe /nologo /O2 /MT /c "$minhook/src/buffer.c" "$minhook/src/hook.c" "$minhook/src/trampoline.c" "$minhook/src/hde/hde64.c"
 if($LASTEXITCODE){throw 'MinHook build failed'}
 $objects=@('buffer.obj','hook.obj','trampoline.obj','hde64.obj')
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /LD "/I$minhook/include" "$PSScriptRoot/tap.cpp" @objects /Fe:sunshine-sonic-object-tap.dll /link bcrypt.lib
 if($LASTEXITCODE){throw 'Sonic capture tap build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "/I$minhook/include" "$PSScriptRoot/test.cpp" @objects /Fe:sonic-source-test.exe /link bcrypt.lib
 if($LASTEXITCODE){throw 'Sonic callback/bridge test build failed'}
 & ./sonic-source-test.exe *> test.log
 if($LASTEXITCODE){throw 'Sonic callback/bridge tests failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /LD "$root/src/spatial_audio/mat_stream_receiver.cpp" /Fe:sunshine-sonic-mat-worker.dll /link runtimeobject.lib ole32.lib bcrypt.lib
 if($LASTEXITCODE){throw 'Dedicated native MAT worker build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "$PSScriptRoot/queue_test.cpp" /Fe:sonic-queue-test.exe
 if($LASTEXITCODE){throw 'Sonic/codec IPC integration build failed'}
 & ./sonic-queue-test.exe "$out/sunshine-sonic-mat-worker.dll" *> queue-test.log
 if($LASTEXITCODE){throw 'Sonic/codec IPC integration failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /DPLUGIN_BRIDGE_QUEUE_TEST "$PSScriptRoot/queue_test.cpp" /Fe:plugin-queue-test.exe
 if($LASTEXITCODE){throw 'Plugin/codec IPC integration build failed'}
 & ./plugin-queue-test.exe "$out/sunshine-sonic-mat-worker.dll" *> plugin-queue-test.log
 if($LASTEXITCODE){throw 'Plugin/codec IPC integration failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /DSPATIAL_USE_EXTERNAL_MINHOOK "/I$minhook/include" /LD "$PSScriptRoot/plugin_tap.cpp" "$root/tools/mat-capture/quantum_v4_recorder.cpp" @objects /Fe:sunshine-plugin-object-tap.dll
 if($LASTEXITCODE){throw 'Plugin paired tap build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /DSPATIAL_USE_EXTERNAL_MINHOOK "/I$minhook/include" "$root/tools/mat-capture/quantum_v4_selftest.cpp" "$root/tools/mat-capture/quantum_v4_recorder.cpp" @objects /Fe:plugin-paired-test.exe
 if($LASTEXITCODE){throw 'Plugin paired tests build failed'}
 & ./plugin-paired-test.exe *> plugin-paired-test.log
 if($LASTEXITCODE){throw 'Plugin paired tests failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "$PSScriptRoot/plugin_capture.cpp" /Fe:sunshine-plugin-mat-capture.exe /link bcrypt.lib
 if($LASTEXITCODE){throw 'Plugin/MAT publisher build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "$PSScriptRoot/capture.cpp" /Fe:sunshine-sonic-mat-capture.exe /link bcrypt.lib
 if($LASTEXITCODE){throw 'Sonic/MAT publisher build failed'}
 & cl.exe /nologo /std:c++20 /O2 /MT /EHsc "$root/tools/sonic-mat-live/spatial_probe.cpp" /Fe:spatial-probe.exe /link ole32.lib uuid.lib propsys.lib
 if($LASTEXITCODE){throw 'Endpoint-specific live source probe build failed'}
 $durationCase=0
 foreach($duration in @('nan','NaN','inf','-inf','0','61')){
  & ./spatial-probe.exe --emit --seconds $duration --gate Local\SunshineMatInvalidDurationTest *> "probe-duration-$durationCase.log"
  if($LASTEXITCODE -ne 2){throw "Probe accepted invalid duration: $duration"}
  ++$durationCase
 }
 # The CLI's expected argument-error code must not become the build's exit code.
 $global:LASTEXITCODE=0
 Get-Content test.log
 Get-Content queue-test.log
}finally{Pop-Location}
$binaries=@(Get-ChildItem $out -File | Where-Object Extension -In '.exe','.dll' | ForEach-Object {@{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash.ToLower()}})
$sourcePaths=@(Get-ChildItem $PSScriptRoot -File | ForEach-Object {$_.FullName})
foreach($file in @('src/spatial_audio/stream_output_protocol.h','src/spatial_audio/object_input_protocol.h','src/spatial_audio/native_encoded_protocol.h','src/spatial_audio/stream_ring_source.h','src/spatial_audio/mat_capture_protocol.h','src/spatial_audio/mat_capture_reader.h','src/spatial_audio/native_mat_pipe_source.h','src/spatial_audio/mat_live_status.h','src/spatial_audio/mat_auto_source.h','src/spatial_audio/mat_stream_receiver.cpp','src/spatial_audio/native_mat_guard.h','tools/mat-live/publisher.h','tools/mat-live/process_helpers.h')){$sourcePaths+=@("$root/$file")}
$sourcePaths+=Get-ChildItem -LiteralPath "$root/tools/mat-capture" -File | ForEach-Object {$_.FullName}
$sourcePaths+=@("$minhook/include/MinHook.h")
$sources=@($sourcePaths | Sort-Object -Unique | ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLower()}})
@{scope='Sonic source and dedicated native MAT worker build; own-process generated tests only';nativeCaptureTested=$false;binaries=$binaries;sources=$sources} | ConvertTo-Json -Depth 5 | Set-Content "$out/build.json" -Encoding utf8
Get-FileHash "$out/sunshine-sonic-mat-capture.exe","$out/sunshine-sonic-object-tap.dll","$out/sunshine-sonic-mat-worker.dll"
