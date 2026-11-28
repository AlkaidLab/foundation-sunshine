@echo off
setlocal DisableDelayedExpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "RULE_NAME=Sunshine"
set "PROGRAM_BIN=%ROOT_DIR%\sunshine.exe"

rem Reuse only equivalent rules for this installation. If inspection fails,
rem retain the old behaviour of adding rules rather than leaving access blocked.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0firewall-rules.ps1" -Program "%PROGRAM_BIN%" -Action inspect
set "FOUND=%ERRORLEVEL%"
if not "%FOUND%"=="10" if not "%FOUND%"=="11" if not "%FOUND%"=="12" if not "%FOUND%"=="13" (
    echo WARNING: Could not inspect existing firewall rules; adding them again.
    set "FOUND=10"
)

set "FAILED=0"
if not "%FOUND%"=="11" if not "%FOUND%"=="13" (
    "%SystemRoot%\System32\netsh.exe" advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=tcp program="%PROGRAM_BIN%" enable=yes
    if errorlevel 1 set "FAILED=1"
)
if not "%FOUND%"=="12" if not "%FOUND%"=="13" (
    "%SystemRoot%\System32\netsh.exe" advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=udp program="%PROGRAM_BIN%" enable=yes
    if errorlevel 1 set "FAILED=1"
)
if "%FAILED%"=="0" (
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0firewall-rules.ps1" -Program "%PROGRAM_BIN%" -Action deduplicate
    if errorlevel 1 (
        echo WARNING: Could not remove duplicate firewall rules.
        set "FAILED=1"
    )
)
> "%~dp0result-firewall.txt" echo %FAILED%
exit /b %FAILED%
