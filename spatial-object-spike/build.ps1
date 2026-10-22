$ErrorActionPreference = 'Stop'
$compilerRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.22621.0'
$env:PATH = "$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = "$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB = "$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $PSScriptRoot
try {
    & cl.exe /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE spatial_probe.cpp /Fe:spatial_probe.exe /link ole32.lib uuid.lib propsys.lib
    if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed' }
    if (Test-Path -LiteralPath '.\spatial_tap.cpp') {
        & cl.exe /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /LD spatial_tap.cpp /Fe:spatial_tap.dll /link ole32.lib uuid.lib propsys.lib
        if ($LASTEXITCODE -ne 0) { throw 'Tap compilation failed' }
    }
    if (Test-Path -LiteralPath '.\capture_launcher.cpp') {
        & cl.exe /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE capture_launcher.cpp /Fe:capture_launcher.exe
        if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
    }
} finally { Pop-Location }
