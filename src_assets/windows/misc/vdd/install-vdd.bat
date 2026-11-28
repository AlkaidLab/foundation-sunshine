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
copy /y nul "%HELPER_LOG%" >nul 2>nul && set "HELPER_LOG=%~dp0vdd-install-last.log"
if not defined HELPER_LOG copy /y nul "%TEMP%\vdd-install-last.log" >nul 2>nul && set "HELPER_LOG=%TEMP%\vdd-install-last.log"
rem When neither log location is writable, run the helper without a redirection: a run without
rem a log is still far better than a helper that never starts.
if defined HELPER_LOG (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vdd-device-helper.ps1" -Action Install -Mode "%INSTALL_MODE%" %PRESERVE_HEALTHY_EXISTING% > "%HELPER_LOG%" 2>&1
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vdd-device-helper.ps1" -Action Install -Mode "%INSTALL_MODE%" %PRESERVE_HEALTHY_EXISTING%
)
set "HELPER_EXIT=%ERRORLEVEL%"
if defined HELPER_LOG type "%HELPER_LOG%"
rem The helper's stdout carries the verdict: some environments (service sessions, redirected
rem consoles) hand back a polluted process exit code even for a completed run, so the log is
rem the authority. 3010 first, then the success marker; anything else is a failure.
findstr /X /C:"VDD_RESTART_REQUIRED=1" "%HELPER_LOG%" >nul
if not errorlevel 1 exit /b 3010
findstr /X /C:"VDD installation completed." "%HELPER_LOG%" >nul
if not errorlevel 1 exit /b 0
findstr /X /C:"VDD_INSTALL_FAILED=1" "%HELPER_LOG%" >nul
if not errorlevel 1 exit /b 1
exit /b %HELPER_EXIT%
