param([Parameter(Mandatory=$true)][string]$Output,[switch]$TestsOnly)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh MAT build output required'}
$vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$version=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if(!$version){throw 'Visual Studio C++ tools are required'}
$major=($version -split '\.')[0]
$generator=(& cmake -E capabilities|ConvertFrom-Json).generators.name | Where-Object {$_ -like "Visual Studio $major *"} | Select-Object -First 1
if(!$generator){throw "CMake does not support installed Visual Studio $major"}
& cmake -S $PSScriptRoot -B $Output -G $generator -A x64 "-DMAT_BUILD_TOOLS=$(!$TestsOnly)"
if($LASTEXITCODE){throw 'MAT configure failed'}
& cmake --build $Output --config Release --parallel 2
if($LASTEXITCODE){throw 'MAT build failed'}
& ctest --test-dir $Output -C Release --output-on-failure
if($LASTEXITCODE){throw 'MAT regressions failed'}
if($TestsOnly){return}
$out=(Resolve-Path -LiteralPath $Output).Path
$case=0
foreach($duration in @('nan','NaN','inf','-inf','0','61')){
 & "$out/spatial-probe.exe" --emit --seconds $duration --gate Local\SunshineMatInvalidDurationTest *> "$out/probe-duration-$case.log"
 if($LASTEXITCODE -ne 2){throw "Probe accepted invalid duration: $duration"}
 ++$case
}
$global:LASTEXITCODE=0
# These hashes are the runtime trust manifest, not a test evidence inventory.
$binaries=@('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll') | ForEach-Object {
 @{name=$_;sha256=(Get-FileHash -LiteralPath "$out/$_").Hash.ToLower()}
}
@{scope='Plugin capture build and own-process software tests';nativeCaptureTested=$false;binaries=@($binaries)} | ConvertTo-Json -Depth 4 | Set-Content "$out/build.json" -Encoding utf8
