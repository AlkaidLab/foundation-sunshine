param([string]$Output="$PSScriptRoot/mat-native-launcher-build01",[string]$ReceiverBuild="$PSScriptRoot/mat-ipc-receiver-build06")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New build directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$receiver=Get-Content -LiteralPath "$ReceiverBuild/build.json" -Raw | ConvertFrom-Json
foreach($entry in $receiver.source_files){if((Get-FileHash -LiteralPath "$PSScriptRoot/$($entry.file)").Hash -ne $entry.sha256){throw 'Frozen receiver source changed'}}
$dll=(Resolve-Path -LiteralPath "$ReceiverBuild/mat_ipc_receiver.dll").Path
if((Get-FileHash -LiteralPath $dll).Hash -ne $receiver.dll_sha256){throw 'Frozen receiver DLL changed'}
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdk='C:/Program Files (x86)/Windows Kits/10';$sdkVersion='10.0.22621.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
$json=(Resolve-Path -LiteralPath "$PSScriptRoot/../../build/codex/_deps/json-src/single_include").Path
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /I $json "$PSScriptRoot/mat_native_launcher.cpp" /Fe:mat-native-launcher.exe /link bcrypt.lib advapi32.lib
 if($LASTEXITCODE){throw 'Launcher build failed'}
 & ./mat-native-launcher.exe --selftest $dll "$PSScriptRoot/cpp-native-v4-replay02/domain-1-mat-object-input.bin" "$out/selftest"
 if($LASTEXITCODE){throw 'Own-child guard/cleanup/output selftests failed'}
 $sources=@();foreach($name in @('mat_native_launcher.cpp','build-mat-native-launcher.ps1','mat_ipc_receiver_api.h','../resident/encoded_protocol.h')){$sources+=@{file=$name;sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/$name").Hash}}
 [ordered]@{scope='Build + new own-child remote entry guard/cleanup tests and synthetic native output validation. No audiodg access, vendor or encoding.'
 executable_sha256=(Get-FileHash -LiteralPath "$out/mat-native-launcher.exe").Hash;source_files=$sources;receiver_dll=$dll;receiver_dll_sha256=$receiver.dll_sha256
 receiver_manifest_sha256=(Get-FileHash -LiteralPath "$ReceiverBuild/build.json").Hash;native_encoder_called=$false;audiodg_accessed=$false;vendor_loaded=$false
 } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
}finally{Pop-Location}
