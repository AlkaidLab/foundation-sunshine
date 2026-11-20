@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0vdd-device-helper.ps1" -Action Uninstall
exit /b %ERRORLEVEL%
