@echo off
setlocal DisableDelayedExpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"
:: Uninstall VB-Cable virtual audio device
:: Requires administrator privileges

:: Check if running as administrator
"%SystemRoot%\System32\net.exe" session >nul 2>&1
if %errorLevel% neq 0 (
    echo Please run this script as administrator
    pause
    exit /b 1
)

:: Remove only the driver packages identified as VB-Cable. The provider's
:: other products, registry settings and program files are not ours to remove.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0uninstall-vsink.ps1"
set "UNINSTALL_RC=%ERRORLEVEL%"
if "%UNINSTALL_RC%"=="3010" (
    echo VB-Cable driver removal requires a restart.
    pause
    exit /b 3010
)
if not "%UNINSTALL_RC%"=="0" (
    echo ERROR: VB-Cable driver removal failed.
    pause
    exit /b 1
)
echo VB-Cable driver removal complete.
pause
exit /b 0
