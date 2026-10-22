param([switch]$CollectorOnly, [switch]$DolbyOnly, [switch]$MetadataOnly, [switch]$StatefulOnly, [ValidatePattern('^[a-zA-Z0-9_-]+\.exe$')][string]$CollectorName='collector.exe')
$ErrorActionPreference = 'Stop'
$compilerRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.39.33519'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.22621.0'
$env:PATH = "$compilerRoot\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = "$compilerRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB = "$compilerRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
Push-Location $PSScriptRoot
try {
    if (-not $CollectorOnly) {
    & cl.exe /nologo /O2 /W3 /MT /c vendor/minhook-1.3.4/src/buffer.c vendor/minhook-1.3.4/src/hook.c vendor/minhook-1.3.4/src/trampoline.c vendor/minhook-1.3.4/src/hde/hde64.c
    if ($LASTEXITCODE -ne 0) { throw 'MinHook build failed' }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /LD /EHsc stateful_tap.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:stateful_tap.dll
    if ($LASTEXITCODE -ne 0) { throw 'Stateful tap build failed' }
    if (-not $StatefulOnly) {
    if (-not $MetadataOnly) {
    if (-not $DolbyOnly) {
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /LD /EHsc engine_tap.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:engine_tap.dll
    if ($LASTEXITCODE -ne 0) { throw 'Engine tap build failed' }
    }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /LD /EHsc dolby_tap.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:dolby_tap.dll
    if ($LASTEXITCODE -ne 0) { throw 'Dolby tap build failed' }
    }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /LD /EHsc metadata_tap.cpp buffer.obj hook.obj trampoline.obj hde64.obj /Fe:metadata_tap.dll
    if ($LASTEXITCODE -ne 0) { throw 'Metadata tap build failed' }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc metadata_selftest.cpp /Fe:metadata_selftest.exe
    if ($LASTEXITCODE -ne 0) { throw 'Metadata selftest build failed' }
    }
    }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc position_cache_test.cpp /Fe:position_cache_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Position cache test build failed' }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc cache_fixture_test.cpp /Fe:cache_fixture_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Cache fixture test build failed' }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc collector.cpp "/Fe:$CollectorName" /link advapi32.lib psapi.lib
    if ($LASTEXITCODE -ne 0) { throw 'Collector build failed' }
    & cl.exe /nologo /std:c++17 /O2 /W4 /MT /EHsc session_audit.cpp /Fe:session_audit.exe /link ole32.lib uuid.lib propsys.lib
    if ($LASTEXITCODE -ne 0) { throw 'Session audit build failed' }
} finally { Pop-Location }
