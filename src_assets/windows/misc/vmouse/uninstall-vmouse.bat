@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================================
rem  Zako Virtual Mouse Driver - Uninstallation Script
rem ============================================================================

rem Get sunshine root directory
for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vmouse"
set "NEFCON=%ROOT_DIR%\tools\vdd\nefconw.exe"

rem Check if nefconw.exe exists
if not exist "%NEFCON%" (
    set "NEFCON=%DIST_DIR%\nefconw.exe"
)

rem Stop Sunshine service to release HID device handle
echo Stopping Sunshine service...
set "SERVICE_WAS_RUNNING=0"
net stop SunshineService >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    set "SERVICE_WAS_RUNNING=1"
    echo Sunshine service stopped.
    timeout /t 2 /nobreak 1>nul
) else (
    echo Sunshine service not running (OK).
)

if exist "%NEFCON%" (
    echo Removing all Virtual Mouse devices...
    :uninstall_remove_loop
    "%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
    if !ERRORLEVEL! EQU 0 goto uninstall_remove_loop

    echo Uninstalling Virtual Mouse driver...
    "%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
)

rem Clean up files
if exist "%DIST_DIR%" (
    rmdir /S /Q "%DIST_DIR%"
)

echo Virtual Mouse driver uninstalled.

rem Restart Sunshine service if it was running before
if "%SERVICE_WAS_RUNNING%"=="1" (
    echo Restarting Sunshine service...
    net start SunshineService >nul 2>&1
)
