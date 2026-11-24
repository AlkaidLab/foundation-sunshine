param([Parameter(Mandatory=$true)][string]$Output,[string]$CMake='cmake')
$ErrorActionPreference='Stop'
$vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$version=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if(!$version){throw 'Visual Studio C++ tools are required for the bundled MAT capture helper'}
$major=($version -split '\.')[0]
$generator=(& $CMake -E capabilities|ConvertFrom-Json).generators.name | Where-Object {$_ -like "Visual Studio $major *"} | Select-Object -First 1
if(!$generator){throw "CMake does not support installed Visual Studio $major"}
& $CMake -S $PSScriptRoot -B $Output -G $generator -A x64 -DBUILD_TESTING=OFF -DMAT_BUILD_TOOLS=ON
if($LASTEXITCODE){throw 'Bundled MAT capture configure failed'}
& $CMake --build $Output --config Release --target sunshine-plugin-mat-capture --parallel 2
if($LASTEXITCODE){throw 'Bundled MAT capture build failed'}
