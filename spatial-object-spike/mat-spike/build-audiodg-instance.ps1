param([string]$Output="$PSScriptRoot/instance-build01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compilerRoot='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot='C:\Program Files (x86)\Windows Kits\10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB="$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $out
try {
  & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /LD "$PSScriptRoot/audiodg_instance_dll.cpp" /Fe:mat_instance_stage_a_v1.dll /link runtimeobject.lib ole32.lib
  if($LASTEXITCODE){throw 'Instance helper build failed'}
  & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "$PSScriptRoot/audiodg_instance_probe.cpp" /Fe:audiodg-instance-probe.exe /link advapi32.lib psapi.lib
  if($LASTEXITCODE){throw 'Instance collector build failed'}
  & ./audiodg-instance-probe.exe --selftest "$out/mat_instance_stage_a_v1.dll"
  if($LASTEXITCODE){throw 'Own-process guard selftest failed'}
}finally{Pop-Location}
