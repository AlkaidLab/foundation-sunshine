param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Use a fresh native MAT build directory'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$out=(New-Item -ItemType Directory -Path $Output).FullName
$compiler='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdk='C:/Program Files (x86)/Windows Kits/10';$version='10.0.26100.0'
$env:PATH="$compiler/bin/Hostx64/x64;"+$env:PATH
$env:INCLUDE="$compiler/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um;$sdk/Include/$version/winrt"
$env:LIB="$compiler/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
Push-Location $out
try {
 $vendor="$root/build/minhook-v1.3.4/src"
 if((git -C "$root/build/minhook-v1.3.4" rev-parse HEAD) -ne "c3fcafdc10146beb5919319d0683e44e3c30d537"){throw "Pinned MinHook v1.3.4 checkout required"}
 & cl.exe /nologo /O2 /W3 /MT /c "$vendor/buffer.c" "$vendor/hook.c" "$vendor/trampoline.c" "$vendor/hde/hde64.c"
 if($LASTEXITCODE){throw 'MinHook compile failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /LD /EHsc /DMAT_LIVE_TAP "/I$root/build/minhook-v1.3.4/include" "$root/spatial-object-spike/resident/encoded_tap.cpp" buffer.obj hook.obj trampoline.obj hde64.obj /Fe:sunshine-mat-tap.dll
 if($LASTEXITCODE){throw 'Pinned native MAT tap compile failed'}
 & cl.exe /nologo /std:c++20 /O2 /W4 /MT /EHsc "$root/spatial-object-spike/mat-live/capture.cpp" /Fe:sunshine-mat-capture.exe
 if($LASTEXITCODE){throw 'Native capture publisher compile failed'}
} finally {Pop-Location}
Get-FileHash "$out/sunshine-mat-tap.dll","$out/sunshine-mat-capture.exe"
