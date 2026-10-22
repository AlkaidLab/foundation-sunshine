param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh pipe regression output required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path;$out=(New-Item -ItemType Directory -Path $Output).FullName
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdk='C:/Program Files (x86)/Windows Kits/10';$version='10.0.26100.0'
$env:INCLUDE="$compiler/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um;$sdk/Include/$version/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
& "$compiler/bin/Hostx64/x64/cl.exe" /nologo /std:c++20 /O2 /W4 /MT /EHsc "$PSScriptRoot/pipe_test.cpp" "/Fo:$out/pipe-test.obj" "/Fe:$out/pipe-test.exe" /link bcrypt.lib *> "$out/build.log"
if($LASTEXITCODE){throw 'Pipe regression compile failed'}
& "$out/pipe-test.exe" *> "$out/test.log"
if($LASTEXITCODE){throw 'Pipe regression failed'}
$paths=@('src/spatial_audio/mat_capture_protocol.h','src/spatial_audio/mat_capture_reader.h','src/spatial_audio/native_mat_pipe_source.h','src/spatial_audio/encoded_audio_source.h','spatial-object-spike/mat-live/capture.cpp','spatial-object-spike/mat-live/pipe_test.cpp','spatial-object-spike/mat-live/build-pipe-test.ps1')
@{result='PASS';nativeCapture=$false;generatedCarrierOnly=$true;scope='Actual private publisher and production record reader; delayed tail, bounded exact-count EOS acknowledgement and negative EOF cases';sources=@($paths|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath "$root/$_").Hash.ToLower()}});testSha256=(Get-FileHash -LiteralPath "$out/pipe-test.exe").Hash.ToLower()}|ConvertTo-Json -Depth 5|Set-Content "$out/proof.json"
Get-Content "$out/test.log"
