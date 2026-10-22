param([string]$Output="$PSScriptRoot/probe-ordered-build01")
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New output directory required; never overwrite frozen source executable'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.39.33519'
$sdk='C:/Program Files (x86)/Windows Kits/10';$version='10.0.22621.0'
$env:INCLUDE="$compiler/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um;$sdk/Include/$version/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
Push-Location $out
try {
 & "$compiler/bin/Hostx64/x64/cl.exe" /nologo /std:c++17 /O2 /W4 /MT /EHsc "$PSScriptRoot/spatial_probe_ordered.cpp" /Fe:spatial-probe-ordered.exe /link ole32.lib uuid.lib propsys.lib
 if($LASTEXITCODE){throw 'Ordered source compile failed'}
 $selftest=& ./spatial-probe-ordered.exe --selftest;$code=$LASTEXITCODE
 $selftest | Set-Content -LiteralPath "$out/selftest.txt" -Encoding utf8
 if($code){throw 'Own-process ordering selftest failed'}
 $selftest
 & ./spatial-probe-ordered.exe --describe
 if($LASTEXITCODE){throw 'Describe failed'}
 & ./spatial-probe-ordered.exe --emit
 if($LASTEXITCODE -ne 2){throw 'Incomplete audio request must be rejected before COM'}
 & ./spatial-probe-ordered.exe --emit --dev 1 --objects 1 --seconds 9 --variant 0
 if($LASTEXITCODE -ne 2){throw 'Out-of-scope duration must be rejected before COM'}
 [ordered]@{scope='Compile + own-process fake selftest only. No audio, device activation, UAC or process attach.'
  source_sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/spatial_probe_ordered.cpp").Hash
  legacy_include_sha256=(Get-FileHash -LiteralPath "$PSScriptRoot/spatial_probe.cpp").Hash
  executable_sha256=(Get-FileHash -LiteralPath "$out/spatial-probe-ordered.exe").Hash
  native_verified=$false;default_collector_changed=$false
 } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$out/build.json" -Encoding utf8
} finally {Pop-Location}
