$ErrorActionPreference='Stop'
$compilerRoot='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot='C:\Program Files (x86)\Windows Kits\10';$sdkVersion='10.0.22621.0'
$env:PATH="$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um"
$env:LIB="$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $PSScriptRoot
try {
    & cl.exe /nologo /O2 /W3 /MT /c vendor/minhook-1.3.4/src/buffer.c vendor/minhook-1.3.4/src/hook.c vendor/minhook-1.3.4/src/trampoline.c vendor/minhook-1.3.4/src/hde/hde64.c
    if($LASTEXITCODE){throw 'MinHook compile failed'}
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /LD /EHsc encoded_tap.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:encoded_tap.dll
    if($LASTEXITCODE){throw 'Encoded tap compile failed'}
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc encoded_probe.cpp /Fe:encoded_probe.exe /link advapi32.lib psapi.lib
    if($LASTEXITCODE){throw 'Encoded probe compile failed'}
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc encoded_selftest.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:encoded_selftest.exe
    if($LASTEXITCODE){throw 'Encoded selftest compile failed'}
} finally {Pop-Location}
