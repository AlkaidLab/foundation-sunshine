param([string]$Output="$PSScriptRoot/mat-ipc-receiver-build06")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdk='C:/Program Files (x86)/Windows Kits/10';$version='10.0.22621.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um;$sdk/Include/$version/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
$json=(Resolve-Path -LiteralPath "$PSScriptRoot/../../build/codex/_deps/json-src/single_include").Path
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$PSScriptRoot/mat_ipc_receiver_dll.cpp" /Fe:mat_ipc_receiver.dll /link runtimeobject.lib ole32.lib bcrypt.lib
 if($LASTEXITCODE){throw 'Receiver DLL build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /I $json "$PSScriptRoot/mat_ipc_receiver_test.cpp" /Fe:mat-ipc-receiver-test.exe
 if($LASTEXITCODE){throw 'Receiver test build failed'}
 & ./mat-ipc-receiver-test.exe --selftest "$out/mat_ipc_receiver.dll" "$out/selftest"
 if($LASTEXITCODE){throw 'Receiver own-process tests failed'}
 $sources=@();foreach($name in @('mat_ipc_receiver_api.h','mat_ipc_receiver_dll.cpp','mat_ipc_receiver_test.cpp','build-mat-ipc-receiver.ps1','../resident/encoded_protocol.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
 [ordered]@{scope='Build + ordinary own-process fake/host-guard tests only. No elevation, deployment, audiodg attachment, audio activation or vendor load.';source_files=$sources
 dll_sha256=(Get-FileHash -LiteralPath "$out/mat_ipc_receiver.dll").Hash;executable_sha256=(Get-FileHash -LiteralPath "$out/mat-ipc-receiver-test.exe").Hash
 native_encoder_called=$false;vendor_loaded=$false;process_attached=$false;production_default_changed=$false
 } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
}finally{Pop-Location}
