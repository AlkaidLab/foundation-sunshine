@echo off
setlocal DisableDelayedExpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"
set "PROGRAM_BIN=%ROOT_DIR%\sunshine.exe"

rem Preserve rules with custom conditions and rules for other installations.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0firewall-rules.ps1" -Program "%PROGRAM_BIN%" -Action remove
exit /b %ERRORLEVEL%
