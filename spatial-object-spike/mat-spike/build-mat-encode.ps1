param([string]$Output="$PSScriptRoot/encode-build03",[ValidatePattern('^[a-zA-Z0-9_-]+[.]dll$')][string]$HelperName='mat_encode_stage_b_v3.dll')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compilerRoot='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot='C:\Program Files (x86)\Windows Kits\10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB="$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$PSScriptRoot/mat_encode_dll.cpp" "/Fe:$HelperName" /link runtimeobject.lib ole32.lib
 if($LASTEXITCODE){throw 'Encode helper build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc "$PSScriptRoot/mat_encode_probe.cpp" /Fe:mat-encode-probe.exe /link advapi32.lib psapi.lib
 if($LASTEXITCODE){throw 'Encode collector build failed'}
 & ./mat-encode-probe.exe --selftest "$out/$HelperName"
 if($LASTEXITCODE){throw 'Encode helper guard failed'}
}finally{Pop-Location}
