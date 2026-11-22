@echo off
setlocal DisableDelayedExpansion
set "COMPONENT_MODE=0"
if /i "%~1"=="--component" set "COMPONENT_MODE=1"
if not defined SystemRoot if defined windir set "SystemRoot=%windir%"
if not defined SystemRoot (
    echo ERROR: Windows system directory is unavailable.
    call :pause_if_interactive
    exit /b 1
)
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
:: Uninstall VB-Cable virtual audio device
:: Requires administrator privileges

:: Check if running as administrator
if not exist "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" (
    echo ERROR: Windows PowerShell is unavailable.
    call :pause_if_interactive
    exit /b 1
)
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') { exit 2 }; try { $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent()); if ($p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 0 } else { exit 1 } } catch { exit 2 }" >nul 2>&1
set "ADMIN_CHECK_RC=%ERRORLEVEL%"
if "%ADMIN_CHECK_RC%"=="2" (
    echo ERROR: Windows PowerShell is restricted or could not check administrator privileges.
    call :pause_if_interactive
    exit /b 1
)
if not "%ADMIN_CHECK_RC%"=="0" (
    echo Please run this script as administrator
    call :pause_if_interactive
    exit /b 1
)

:: Remove only the driver packages identified as VB-Cable. The provider's
:: other products, registry settings and program files are not ours to remove.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0uninstall-vsink.ps1"
set "UNINSTALL_RC=%ERRORLEVEL%"
if "%UNINSTALL_RC%"=="3010" (
    echo Windows reported a pending removal. After restarting, run this script again if VB-Cable is still listed.
    call :pause_if_interactive
    exit /b 3010
)
if not "%UNINSTALL_RC%"=="0" (
    echo ERROR: VB-Cable driver removal failed.
    call :pause_if_interactive
    exit /b %UNINSTALL_RC%
)
echo VB-Cable driver removal complete.
call :pause_if_interactive
exit /b 0

:pause_if_interactive
if not "%COMPONENT_MODE%"=="1" pause
exit /b 0
