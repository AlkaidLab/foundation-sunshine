param([string]$Output="$PSScriptRoot/lifecycle-build01",[string]$InputDirectory="$PSScriptRoot/lifecycle-input01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$inputFile=(Resolve-Path -LiteralPath (Join-Path $InputDirectory 'input.bin')).Path
$compilerRoot='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot='C:\Program Files (x86)\Windows Kits\10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB="$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $out
try{
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc /LD "$PSScriptRoot/mat_lifecycle_dll.cpp" /Fe:mat_lifecycle_v1.dll /link runtimeobject.lib ole32.lib
 if($LASTEXITCODE){throw 'Lifecycle helper build failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /wd4324 /wd4505 /MT /EHsc "$PSScriptRoot/mat_lifecycle_probe.cpp" /Fe:mat-lifecycle-probe.exe /link advapi32.lib psapi.lib
 if($LASTEXITCODE){throw 'Lifecycle collector build failed'}
 & ./mat-lifecycle-probe.exe --selftest "$out/mat_lifecycle_v1.dll" $inputFile
 if($LASTEXITCODE){throw 'Lifecycle guard/input failed'}
}finally{Pop-Location}
