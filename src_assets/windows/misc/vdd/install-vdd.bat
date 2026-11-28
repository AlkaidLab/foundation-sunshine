@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

set "INSTALL_MODE=Run"
set "PRESERVE_HEALTHY_EXISTING="
for %%A in (%*) do (
    if /i "%%~A"=="--resolve-only" set "INSTALL_MODE=ResolveOnly"
    if /i "%%~A"=="--probe-only" set "INSTALL_MODE=ProbeOnly"
    if /i "%%~A"=="--preserve-healthy-existing" set "PRESERVE_HEALTHY_EXISTING=-PreserveHealthyExisting"
)

rem The helper's answer is the only place a failure names its reason. Keep the full
rem output next to this script when the scripts folder is writable, and in the
rem caller's temp folder when it is not, then echo it for the console.
set "HELPER_LOG=%~dp0vdd-install-last.log"
2>nul copy /y nul "%HELPER_LOG%" >nul
if errorlevel 1 set "HELPER_LOG=%TEMP%\vdd-install-last.log"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vdd-device-helper.ps1" -Action Install -Mode "%INSTALL_MODE%" %PRESERVE_HEALTHY_EXISTING% > "%HELPER_LOG%" 2>&1
set "HELPER_EXIT=%ERRORLEVEL%"
type "%HELPER_LOG%"
exit /b %HELPER_EXIT%
