@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

rem Set the service to auto-start
"%SystemRoot%\System32\sc.exe" config SunshineService start= auto
set "STEP_RC=%ERRORLEVEL%"
> "%~dp0result-autostart.txt" echo %STEP_RC%
exit /b %STEP_RC%
