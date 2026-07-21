@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"

set "INSTALL_MODE=Run"
for %%A in (%*) do (
    if /i "%%~A"=="--resolve-only" set "INSTALL_MODE=ResolveOnly"
    if /i "%%~A"=="--probe-only" set "INSTALL_MODE=ProbeOnly"
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vdd-device-helper.ps1" -Action Install -Mode "%INSTALL_MODE%"
exit /b %ERRORLEVEL%
