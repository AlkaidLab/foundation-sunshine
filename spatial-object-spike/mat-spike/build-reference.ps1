param([string]$Output="$PSScriptRoot/reference-build01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compilerRoot='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot='C:\Program Files (x86)\Windows Kits\10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB="$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
$ref="$PSScriptRoot/reference/spatial-audio-lab/src"
Push-Location $out
try {
  & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$ref" "$PSScriptRoot/reference_check.cpp" "$ref/mat_format.cpp" /Fe:reference-check.exe
  if($LASTEXITCODE){throw 'Reference parser build failed'}
}finally {Pop-Location}
