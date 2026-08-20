@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================================
rem  Zako Virtual Mouse Driver - Uninstallation Script
rem ============================================================================

rem Get sunshine root directory
for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vmouse"
set "NEFCON=%ROOT_DIR%\tools\nefconw.exe"

rem Check if nefconw.exe exists
if not exist "%NEFCON%" (
    set "NEFCON=%ROOT_DIR%\tools\vdd\nefconw.exe"
)

rem Stop Sunshine service to release HID device handle.
rem Use sc + taskkill instead of `net stop` to avoid up to 30s SCM blocking.
echo Stopping Sunshine service...
set "SERVICE_WAS_RUNNING=0"
sc query SunshineService >nul 2>&1
if not errorlevel 1 (
    rem Service exists; check if it's running
    sc query SunshineService | find /I "RUNNING" >nul 2>&1
    if not errorlevel 1 (
        set "SERVICE_WAS_RUNNING=1"
        sc stop SunshineService >nul 2>&1
    )
    rem Force-kill the service binary so it releases the HID handle quickly.
    taskkill /f /im sunshinesvc.exe >nul 2>&1
    timeout /t 1 /nobreak >nul 2>&1
    echo Sunshine service stopped.
) else (
    echo Sunshine service not installed, OK.
)

if not exist "%NEFCON%" goto skip_nefcon_uninstall

echo Removing all Virtual Mouse devices via nefcon...
set "NEFCON_REMOVED=0"
set "NEFCON_ITERS=0"
set "NEFCON_MAX_ITERS=20"
set "NEFCON_REMOVE_LOG=%temp%\sunshine-vmouse-uninstall-nefcon-remove.log"
call :count_vmouse_devices REMAINING_BEFORE
echo Found !REMAINING_BEFORE! existing Virtual Mouse device node^(s^).
:uninstall_remove_loop
if !REMAINING_BEFORE! LEQ 0 goto after_remove_loop
if !NEFCON_ITERS! GEQ !NEFCON_MAX_ITERS! (
    echo WARN: Reached max remove iterations !NEFCON_MAX_ITERS!; stopping nefcon cleanup.
    goto after_remove_loop
)
set /a NEFCON_ITERS+=1
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da > "%NEFCON_REMOVE_LOG%" 2>&1
set "NEFCON_REMOVE_RC=!ERRORLEVEL!"
for %%I in ("%NEFCON_REMOVE_LOG%") do if %%~zI GTR 0 type "%NEFCON_REMOVE_LOG%"
call :count_vmouse_devices REMAINING_AFTER
if !REMAINING_AFTER! LSS !REMAINING_BEFORE! (
    set /a REMOVED_NOW=REMAINING_BEFORE-REMAINING_AFTER
    set /a NEFCON_REMOVED+=REMOVED_NOW
    echo Removed !REMOVED_NOW! device node^(s^), !REMAINING_AFTER! remaining...
    set "REMAINING_BEFORE=!REMAINING_AFTER!"
    timeout /t 1 /nobreak >nul
    goto uninstall_remove_loop
)
if !NEFCON_REMOVE_RC! GEQ 1 (
    echo WARN: nefcon returned !NEFCON_REMOVE_RC! with !REMAINING_AFTER! device node^(s^) still present; falling back to pnputil cleanup.
) else (
    echo WARN: nefcon did not reduce the remaining device count; falling back to pnputil cleanup.
)
set "REMAINING_BEFORE=!REMAINING_AFTER!"
:after_remove_loop
if exist "%NEFCON_REMOVE_LOG%" del /f /q "%NEFCON_REMOVE_LOG%" >nul 2>&1
echo Removed !NEFCON_REMOVED! device node(s) via nefcon.

echo Uninstalling Virtual Mouse driver...
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
:skip_nefcon_uninstall

rem Fallback: use pnputil to remove any remaining device instances
rem This catches ghost devices that nefcon may fail to handle
echo Checking for remaining Virtual Mouse devices...
set "PNPUTIL_REMOVED=0"
for /f "tokens=*" %%d in ('powershell -NoProfile -Command ^
    "Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' } | ForEach-Object { $_.InstanceId }"') do (
    echo Removing remaining device: %%d
    pnputil /remove-device "%%d" >nul 2>&1
    set /a PNPUTIL_REMOVED+=1
)
call :count_vmouse_devices REMAINING_AFTER_PNPUTIL
if !PNPUTIL_REMOVED! GTR 0 (
    echo Removed !PNPUTIL_REMOVED! remaining device^(s^) via pnputil.
) else (
    echo No remaining devices found.
)
if !REMAINING_AFTER_PNPUTIL! GTR 0 (
    echo WARN: !REMAINING_AFTER_PNPUTIL! device node^(s^) still remain after pnputil cleanup.
)

rem Clean up driver package from DriverStore (locale-independent)
powershell -NoProfile -Command "Get-ChildItem ($env:SystemRoot + '\INF\oem*.inf') -ErrorAction SilentlyContinue | Where-Object { Select-String -Path $_.FullName -Pattern 'ZakoVirtualMouse' -Quiet } | ForEach-Object { Write-Host ('Removing driver package: ' + $_.Name); pnputil /delete-driver $_.Name /force | Out-Null }"

rem Clean up files
if exist "%DIST_DIR%" (
    rmdir /S /Q "%DIST_DIR%"
)

echo Virtual Mouse driver uninstalled.

rem Restart Sunshine service if it was running before and still exists.
rem In the full uninstaller flow the service has already been deleted by
rem uninstall-service.bat, so this only matters when the script is run
rem standalone (e.g. user-initiated driver reset).
if "!SERVICE_WAS_RUNNING!"=="1" (
    sc query SunshineService >nul 2>&1
    if not errorlevel 1 (
        echo Restarting Sunshine service...
        sc start SunshineService >nul 2>&1
    )
)

exit /b 0

:count_vmouse_devices
setlocal
set "COUNT=0"
for /f %%C in ('powershell -NoProfile -Command ^
    "$devices = Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\ZakoVirtualMouse' }; @($devices).Count"') do set "COUNT=%%C"
endlocal & set "%~1=%COUNT%"
exit /b 0
