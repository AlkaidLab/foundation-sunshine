@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================================
rem  Zako Virtual Mouse Driver - Installation Script
rem  UMDF 2.x HID Minidriver for hardware-level virtual mouse
rem ============================================================================

set "DRIVER_DIR=%~dp0driver"
if not exist "%DRIVER_DIR%\ZakoVirtualMouse.inf" (
    rem Release archives may place the driver files next to this script.
    set "DRIVER_DIR=%~dp0"
)

rem Get sunshine root directory
for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vmouse"
set "NEFCON=%ROOT_DIR%\tools\nefconw.exe"

rem Check if nefconw.exe exists
if not exist "%NEFCON%" (
    rem Fallback: try VDD component location
    set "NEFCON=%ROOT_DIR%\tools\vdd\nefconw.exe"
)
if not exist "%NEFCON%" (
    echo ERROR: nefconw.exe not found.
    echo        Expected at: %ROOT_DIR%\tools\nefconw.exe
    exit /b 1
)

rem ----------------------------------------------------------------------------
rem  Sanity check: refuse to install an unstamped INF (UmdfLibraryVersion still
rem  contains the literal "$UMDFVERSION$" placeholder). WUDF coinstaller will
rem  fail with error 87 (ERROR_INVALID_PARAMETER) at DIF_INSTALLDEVICE in that
rem  case, leaving an orphan ROOT\HIDCLASS node and a broken DriverStore entry.
rem ----------------------------------------------------------------------------
findstr /C:"$UMDFVERSION$" "%DRIVER_DIR%\ZakoVirtualMouse.inf" >nul 2>&1
if not errorlevel 1 (
    echo ERROR: Bundled ZakoVirtualMouse.inf is not stamped ^(UmdfLibraryVersion=$UMDFVERSION$^).
    echo        This package will fail to install with error 87.
    echo        Rebuild the driver via stampinf or replace with a stamped package.
    echo        Source: %DRIVER_DIR%\ZakoVirtualMouse.inf
    exit /b 87
)

rem Skip expensive PnP and DriverStore cleanup on a clean installation.
set "VMOUSE_CLEANUP_REQUIRED=0"
if exist "%DIST_DIR%\ZakoVirtualMouse.inf" set "VMOUSE_CLEANUP_REQUIRED=1"
call :count_vmouse_devices EXISTING_VIRTUAL_MOUSE_DEVICES
if !EXISTING_VIRTUAL_MOUSE_DEVICES! GTR 0 set "VMOUSE_CLEANUP_REQUIRED=1"

rem Copy driver files to target directory
if exist "%DIST_DIR%" (
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"
copy "%DRIVER_DIR%\*.*" "%DIST_DIR%"

set "SERVICE_WAS_RUNNING=0"
if "!VMOUSE_CLEANUP_REQUIRED!"=="1" goto :cleanup_existing_vmouse
echo No existing Virtual Mouse device or payload found; skipping legacy cleanup.
goto :after_existing_vmouse_cleanup

rem ============================================================================
rem  Stop Sunshine service to release HID device handle
rem ============================================================================

:cleanup_existing_vmouse
rem Use sc + taskkill (non-blocking) instead of `net stop`, which can block
rem for up to 30 seconds on a stuck stop handler.
echo Stopping Sunshine service...
sc query SunshineService >nul 2>&1
if not errorlevel 1 (
    sc query SunshineService | find /I "RUNNING" >nul 2>&1
    if not errorlevel 1 (
        set "SERVICE_WAS_RUNNING=1"
        sc stop SunshineService >nul 2>&1
    )
    taskkill /f /im sunshinesvc.exe >nul 2>&1
    echo Sunshine service stopped.
) else (
    echo Sunshine service not installed, OK.
)

rem ============================================================================
rem  Cleanup existing installation
rem ============================================================================

echo Cleaning up existing Virtual Mouse driver...

rem Remove ALL existing device nodes. Drive the loop by the actual remaining
rem PnP device count rather than nefcon's exit code, because some nefcon builds
rem print a delete error yet still return success.
set "CLEANUP_COUNT=0"
set "CLEANUP_ITERS=0"
set "CLEANUP_MAX=20"
set "NEFCON_REMOVE_LOG=%temp%\sunshine-vmouse-install-nefcon-remove.log"
call :count_vmouse_devices REMAINING_BEFORE
echo Found !REMAINING_BEFORE! existing Virtual Mouse device node^(s^).
:remove_loop
if !REMAINING_BEFORE! LEQ 0 goto after_remove_loop
if !CLEANUP_ITERS! GEQ !CLEANUP_MAX! (
    echo WARN: Reached max remove iterations !CLEANUP_MAX!; stopping nefcon cleanup.
    goto after_remove_loop
)
set /a CLEANUP_ITERS+=1
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da > "%NEFCON_REMOVE_LOG%" 2>&1
set "NEFCON_REMOVE_RC=!ERRORLEVEL!"
for %%I in ("%NEFCON_REMOVE_LOG%") do if %%~zI GTR 0 type "%NEFCON_REMOVE_LOG%"
call :count_vmouse_devices REMAINING_AFTER
if !REMAINING_AFTER! LSS !REMAINING_BEFORE! (
    set /a REMOVED_NOW=REMAINING_BEFORE-REMAINING_AFTER
    set /a CLEANUP_COUNT+=REMOVED_NOW
    echo Removed !REMOVED_NOW! device node^(s^), !REMAINING_AFTER! remaining...
    set "REMAINING_BEFORE=!REMAINING_AFTER!"
    goto remove_loop
)
if !NEFCON_REMOVE_RC! GEQ 1 (
    echo WARN: nefcon returned !NEFCON_REMOVE_RC! with !REMAINING_AFTER! device node^(s^) still present; falling back to pnputil cleanup.
) else (
    echo WARN: nefcon did not reduce the remaining device count; falling back to pnputil cleanup.
)
set "REMAINING_BEFORE=!REMAINING_AFTER!"
:after_remove_loop
if exist "%NEFCON_REMOVE_LOG%" del /f /q "%NEFCON_REMOVE_LOG%" >nul 2>&1
echo Removed !CLEANUP_COUNT! device node(s) via nefcon.

rem Fallback: use pnputil to remove any remaining device instances
for /f "tokens=*" %%d in ('powershell -NoProfile -Command ^
    "Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' } | ForEach-Object { $_.InstanceId }"') do (
    echo Removing remaining device: %%d
    pnputil /remove-device "%%d" >nul 2>&1
)
call :wait_for_vmouse_removal 20
call :count_vmouse_devices REMAINING_AFTER_PNPUTIL
if !REMAINING_AFTER_PNPUTIL! GTR 0 (
    echo WARN: !REMAINING_AFTER_PNPUTIL! device node^(s^) still remain after pnputil cleanup.
) else (
    echo All existing device nodes removed.
)

rem Uninstall previous driver
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
if not errorlevel 1 (
    echo Successfully uninstalled previous driver
) else (
    echo No previous driver found, OK.
)

rem Clean up stale driver packages from DriverStore (locale-independent)
powershell -NoProfile -Command "Get-ChildItem ($env:SystemRoot + '\INF\oem*.inf') -ErrorAction SilentlyContinue | Where-Object { Select-String -Path $_.FullName -Pattern 'ZakoVirtualMouse' -Quiet } | ForEach-Object { Write-Host ('Removing stale driver package: ' + $_.Name); pnputil /delete-driver $_.Name /force | Out-Null }"

rem ============================================================================
rem  Install Certificate and Driver
rem ============================================================================

:after_existing_vmouse_cleanup
rem Install certificate to Trusted Root and Trusted Publisher stores
set "CERTIFICATE=%DIST_DIR%\ZakoVirtualMouse.cer"
if exist "%CERTIFICATE%" (
    echo Installing driver certificate...
    certutil -addstore -f root "%CERTIFICATE%"
    certutil -addstore -f TrustedPublisher "%CERTIFICATE%"
)

rem Create device node and install driver
echo Installing Virtual Mouse driver...
set "INSTALL_RESULT=0"
"%NEFCON%" --create-device-node --hardware-id Root\ZakoVirtualMouse --class-name HIDClass --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
"%NEFCON%" --install-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
set "INSTALL_RESULT=!ERRORLEVEL!"
if not "!INSTALL_RESULT!"=="0" (
    echo nefcon install failed with error !INSTALL_RESULT!, trying pnputil fallback...
    pnputil /add-driver "%DIST_DIR%\ZakoVirtualMouse.inf" /install
    set "INSTALL_RESULT=!ERRORLEVEL!"
)

if "!INSTALL_RESULT!"=="0" (
    echo Virtual Mouse driver installation completed successfully!
) else (
    echo Virtual Mouse driver installation failed with error !INSTALL_RESULT!
    echo Recent SetupAPI lines for ZakoVirtualMouse:
    powershell -NoProfile -Command "Select-String -Path ($env:SystemRoot + '\INF\setupapi.dev.log') -Pattern 'ZakoVirtualMouse','Root\\ZakoVirtualMouse','ROOT\\HIDCLASS' -CaseSensitive:$false -ErrorAction SilentlyContinue | Select-Object -Last 100 | ForEach-Object { $_.Line }"
    echo Rolling back: removing device node...
    "%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
    for /f "tokens=*" %%d in ('powershell -NoProfile -Command ^
        "Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' } | ForEach-Object { $_.InstanceId }"') do (
        echo Removing rollback device: %%d
        pnputil /remove-device "%%d" >nul 2>&1
    )
)

rem ============================================================================
rem  Restart Sunshine service if it was running before
rem ============================================================================

if "!SERVICE_WAS_RUNNING!"=="1" (
    sc query SunshineService >nul 2>&1
    if not errorlevel 1 (
        echo Restarting Sunshine service...
        sc start SunshineService >nul 2>&1
    )
)

exit /b !INSTALL_RESULT!

:count_vmouse_devices
setlocal
set "COUNT=0"
for /f %%C in ('powershell -NoProfile -Command ^
    "$devices = Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' }; @($devices).Count"') do set "COUNT=%%C"
endlocal & set "%~1=%COUNT%"
exit /b 0

:wait_for_vmouse_removal
setlocal enabledelayedexpansion
set "MAX_POLLS=%~1"
for /l %%P in (1,1,!MAX_POLLS!) do (
    call :count_vmouse_devices REMAINING_VIRTUAL_MOUSE_DEVICES
    if !REMAINING_VIRTUAL_MOUSE_DEVICES! LEQ 0 goto :vmouse_removed
    powershell -NoProfile -Command "Start-Sleep -Milliseconds 250"
)
endlocal & exit /b 1

:vmouse_removed
endlocal & exit /b 0
