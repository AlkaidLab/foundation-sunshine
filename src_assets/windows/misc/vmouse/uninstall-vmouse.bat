@echo off
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

rem Stop Sunshine service to release HID device handle
echo Stopping Sunshine service...
set "SERVICE_WAS_RUNNING=0"
net stop SunshineService >nul 2>&1
if not errorlevel 1 (
    set "SERVICE_WAS_RUNNING=1"
    echo Sunshine service stopped.
    timeout /t 2 /nobreak 1>nul
) else (
    echo Sunshine service not running, OK.
)

if not exist "%NEFCON%" goto skip_nefcon_uninstall

echo Removing all Virtual Mouse devices via nefcon...
set "NEFCON_REMOVED=0"
:uninstall_remove_loop
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
if not errorlevel 1 (
    set /a NEFCON_REMOVED+=1
    timeout /t 1 /nobreak >nul
    goto uninstall_remove_loop
)
echo Removed !NEFCON_REMOVED! device node(s) via nefcon.

echo Uninstalling Virtual Mouse driver...
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
:skip_nefcon_uninstall

rem Fallback: use pnputil to remove any remaining device instances
rem This catches ghost devices that nefcon may fail to handle
echo Checking for remaining Virtual Mouse devices...
set "PNPUTIL_REMOVED=0"
for /f "tokens=*" %%d in ('powershell -NoProfile -Command ^
    "Get-PnpDevice -InstanceId 'ROOT\ZAKOVIRTUALMOUSE\*' -ErrorAction SilentlyContinue | ForEach-Object { $_.InstanceId }"') do (
    echo Removing remaining device: %%d
    pnputil /remove-device "%%d" >nul 2>&1
    set /a PNPUTIL_REMOVED+=1
)
if !PNPUTIL_REMOVED! GTR 0 (
    echo Removed !PNPUTIL_REMOVED! remaining device(s) via pnputil.
) else (
    echo No remaining devices found.
)

rem Clean up driver package from DriverStore (locale-independent)
for /f "tokens=*" %%p in ('powershell -NoProfile -Command ^
    "Get-ChildItem \"$env:SystemRoot\INF\oem*.inf\" -ErrorAction SilentlyContinue | Where-Object { Select-String -Path $_.FullName -Pattern 'ZakoVirtualMouse' -Quiet } | ForEach-Object { $_.Name }"') do (
    echo Removing driver package: %%p
    pnputil /delete-driver "%%p" /force >nul 2>&1
)

rem Clean up files
if exist "%DIST_DIR%" (
    rmdir /S /Q "%DIST_DIR%"
)

echo Virtual Mouse driver uninstalled.

rem Restart Sunshine service if it was running before
if "!SERVICE_WAS_RUNNING!"=="1" (
    echo Restarting Sunshine service...
    net start SunshineService >nul 2>&1
)
