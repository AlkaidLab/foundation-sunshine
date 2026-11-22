@echo off
setlocal DisableDelayedExpansion
set "COMPONENT_MODE=0"
if /i "%~1"=="--component" (
    set "COMPONENT_MODE=1"
    set "VB_ARCHIVE_BASE64=%~2"
    if "%~2"=="" exit /b 1
    if not "%~3"=="" exit /b 1
)
if not defined SystemRoot if defined windir set "SystemRoot=%windir%"
if not defined SystemRoot (
    echo ERROR: Windows system directory is unavailable.
    call :pause_if_interactive
    exit /b 1
)
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"
:: The Windows PowerShell child must not load incompatible PowerShell 7 modules.
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"
if not exist "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" (
    echo ERROR: Windows PowerShell is unavailable.
    call :pause_if_interactive
    exit /b 1
)
if /i "%~1"=="--check" goto :check_only
:: Check for administrator privileges
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

:: Check the live PnP device, not Win32_PnPSignedDriver's cached inventory.
call :check_vb_cable 0
set "VB_CHECK_RC=%ERRORLEVEL%"
if "%VB_CHECK_RC%"=="0" (
    echo VB-Cable driver is already installed
    call :pause_if_interactive
    exit /b 0
)
if not "%VB_CHECK_RC%"=="1" (
    echo ERROR: VB-Cable device is unavailable or could not be checked.
    call :pause_if_interactive
    exit /b 1
)
:: Pack43's x64 setup cannot install an ARM64 kernel driver. Keep --check
:: version- and architecture-independent; this guard applies only to installation.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; try { $source = 'using System; using System.Runtime.InteropServices; public static class VbNativeArchitecture { [DllImport(@DLL@, SetLastError=true)] public static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine); }'.Replace('@DLL@', ([string][char]34 + 'kernel32.dll' + [char]34)); Add-Type -TypeDefinition $source; $processMachine = [uint16]0; $nativeMachine = [uint16]0; if ([VbNativeArchitecture]::IsWow64Process2([Diagnostics.Process]::GetCurrentProcess().Handle, [ref]$processMachine, [ref]$nativeMachine) -and $nativeMachine -eq 0x8664) { exit 0 }; exit 1 } catch { exit 2 }" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Pack43 requires x64 Windows, or its architecture could not be checked.
    call :pause_if_interactive
    exit /b 1
)
call :check_driver_stopped
if errorlevel 1 (
    echo ERROR: The previous VB-Cable driver is still running or its state is unavailable.
    call :pause_if_interactive
    exit /b 1
)

:: Set variables
set "installer=VBCABLE_Driver_Pack43.zip"
set "download_url=https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip"
set "installer_sha256=66FD0A4D9F4896FF41632B7E3D53892C085C4561F53E8AE8D0F0BC10EEDD1CDD"
if not defined TEMP if defined TMP set "TEMP=%TMP%"
if not defined TEMP set "TEMP=%SystemRoot%\Temp"
if "%COMPONENT_MODE%"=="1" (
    :: The component manager stages this script in an administrator-only directory.
    set "temp_dir=%~dp0payload"
) else (
    set "temp_dir=%TEMP%\Sunshine-VBCable-%RANDOM%-%RANDOM%"
)
set "FAILURE_RC=1"

:: Use a new directory so files left by a previous attempt cannot be reused.
mkdir "%temp_dir%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Could not create the temporary directory.
    call :pause_if_interactive
    exit /b 1
)

:: Use the component's prepared archive, a manually placed archive, or download it.
if "%COMPONENT_MODE%"=="1" (
    echo Using the prepared VB-Cable driver...
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; $path = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($env:VB_ARCHIVE_BASE64)); Copy-Item -LiteralPath $path -Destination (Join-Path $env:temp_dir $env:installer) -ErrorAction Stop"
) else if exist "%~dp0vbcable\%installer%" (
    echo Using the local VB-Cable driver...
    copy /y "%~dp0vbcable\%installer%" "%temp_dir%\%installer%" >nul
) else (
    echo Downloading VB-Cable driver...
    rem WebClient accepts a literal path; Windows PowerShell 5.1 treats brackets in Invoke-WebRequest -OutFile as wildcards.
    "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; $client = New-Object System.Net.WebClient; try { $client.DownloadFile($env:download_url, (Join-Path $env:temp_dir $env:installer)) } finally { $client.Dispose() }"
)
if errorlevel 1 (
    echo ERROR: Could not obtain the VB-Cable driver.
    goto :failed
)

:: Verify the complete archive before extracting or running anything from it.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $env:temp_dir $env:installer)).Hash -ne $env:installer_sha256) { exit 1 }"
if errorlevel 1 (
    echo ERROR: VB-Cable driver archive verification failed.
    goto :failed
)

:: Extract files
echo Extracting files...
:: DestinationPath is wildcard-aware in Windows PowerShell 5.1.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; Expand-Archive -LiteralPath (Join-Path $env:temp_dir $env:installer) -DestinationPath ([System.Management.Automation.WildcardPattern]::Escape($env:temp_dir)) -Force"
if errorlevel 1 (
    echo ERROR: Could not extract the VB-Cable driver.
    goto :failed
)

:: Install driver
echo Installing VB-Cable driver...
if not exist "%temp_dir%\VBCABLE_Setup_x64.exe" (
    echo ERROR: The VB-Cable setup program is missing.
    goto :failed
)
:: Device removal can be pending even when the device no longer appears.
call :check_vb_cable 0
if not "%ERRORLEVEL%"=="1" (
    echo ERROR: VB-Cable appeared or could not be checked before installation.
    goto :failed
)
call :check_driver_stopped
if errorlevel 1 (
    echo ERROR: The previous VB-Cable driver is still running or its state is unavailable.
    goto :failed
)
start /wait "" "%temp_dir%\VBCABLE_Setup_x64.exe" -i -h
set "SETUP_RC=%ERRORLEVEL%"
if "%SETUP_RC%"=="3010" (
    echo ERROR: The VB-Cable setup reported that a restart is required.
    set "FAILURE_RC=3010"
    goto :failed
)
if not "%SETUP_RC%"=="0" (
    echo ERROR: The VB-Cable setup program failed.
    goto :failed
)

:: A successful process exit alone does not prove the driver appeared.
call :check_vb_cable 15
if errorlevel 1 (
    echo ERROR: VB-Cable was not detected after setup.
    goto :failed
)

:: Clean up temp files
echo Cleaning up temporary files...
rd /s /q "%temp_dir%"

echo VB-Cable driver installation completed!
call :pause_if_interactive
exit /b 0

:failed
rd /s /q "%temp_dir%" >nul 2>&1
call :pause_if_interactive
exit /b %FAILURE_RC%

:check_only
call :check_vb_cable 0
exit /b %ERRORLEVEL%

:check_vb_cable
set "VB_CHECK_WAIT=%~1"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; try { $seconds = [int]$env:VB_CHECK_WAIT; $deadline = [DateTime]::UtcNow.AddSeconds($seconds); do { $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.Class -eq 'MEDIA' -and $_.Manufacturer -eq 'VB-Audio Software' -and @($_.HardwareID) -contains 'VBAudioVACWDM' }); if (@($devices | Where-Object { $_.Status -eq 'OK' }).Count) { exit 0 }; if ($seconds -eq 0) { if ($devices.Count) { exit 2 } else { exit 1 } }; Start-Sleep -Milliseconds 500 } while ([DateTime]::UtcNow -lt $deadline); exit 1 } catch { exit 3 }" >nul 2>&1
exit /b %ERRORLEVEL%

:check_driver_stopped
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; try { $drivers = @(Get-CimInstance -ClassName Win32_SystemDriver -ErrorAction Stop | Where-Object { $_.Name -eq 'VBAudioVACMME' }); if ($drivers.Count -eq 0 -or ($drivers.Count -eq 1 -and $drivers[0].State -eq 'Stopped')) { exit 0 }; exit 1 } catch { exit 2 }" >nul 2>&1
exit /b %ERRORLEVEL%

:pause_if_interactive
if not "%COMPONENT_MODE%"=="1" pause
exit /b 0
