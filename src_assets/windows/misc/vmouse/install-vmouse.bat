@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ============================================================================
rem  Zako Virtual Mouse Driver - Installation Script
rem  UMDF 2.x HID Minidriver for hardware-level virtual mouse
rem ============================================================================

set "DRIVER_DIR=%~dp0driver"

rem Get sunshine root directory
for %%I in ("%~dp0..\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vmouse"
set "NEFCON=%ROOT_DIR%\tools\vdd\nefconw.exe"

rem Check if nefconw.exe exists (shared with VDD component)
if not exist "%NEFCON%" (
    rem Try vmouse's own copy
    set "NEFCON=%DIST_DIR%\nefconw.exe"
)
if not exist "%NEFCON%" (
    echo ERROR: nefconw.exe not found. Please install the Virtual Display Driver component first,
    echo        or copy nefconw.exe to %DIST_DIR%
    exit /b 1
)

rem Copy driver files to target directory
if exist "%DIST_DIR%" (
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"
copy "%DRIVER_DIR%\*.*" "%DIST_DIR%"

rem ============================================================================
rem  Stop Sunshine service to release HID device handle
rem ============================================================================

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

rem ============================================================================
rem  Cleanup existing installation
rem ============================================================================

echo Cleaning up existing Virtual Mouse driver...

rem Remove ALL existing device nodes (loop until none remain)
:remove_loop
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVirtualMouse --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
if not errorlevel 1 (
    echo Removed a device node, checking for more...
    goto remove_loop
)
echo All existing device nodes removed.

timeout /t 2 /nobreak 1>nul

rem Uninstall previous driver
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"
if not errorlevel 1 (
    echo Successfully uninstalled previous driver
) else (
    echo No previous driver found, OK.
)

timeout /t 3 /nobreak 1>nul

rem ============================================================================
rem  Install Certificate and Driver
rem ============================================================================

rem Install certificate to Trusted Root and Trusted Publisher stores
set "CERTIFICATE=%DIST_DIR%\ZakoVirtualMouse.cer"
if exist "%CERTIFICATE%" (
    echo Installing driver certificate...
    certutil -addstore -f root "%CERTIFICATE%"
    certutil -addstore -f TrustedPublisher "%CERTIFICATE%"
)

rem Create device node and install driver
echo Installing Virtual Mouse driver...
"%NEFCON%" --create-device-node --hardware-id Root\ZakoVirtualMouse --class-name HIDClass --class-guid 745a17a0-74d3-11d0-b6fe-00a0c90f57da
"%NEFCON%" --install-driver --inf-path "%DIST_DIR%\ZakoVirtualMouse.inf"

if not errorlevel 1 (
    echo Virtual Mouse driver installation completed successfully!
) else (
    echo Virtual Mouse driver installation failed with error !ERRORLEVEL!
)

rem ============================================================================
rem  Restart Sunshine service if it was running before
rem ============================================================================

if "!SERVICE_WAS_RUNNING!"=="1" (
    echo Restarting Sunshine service...
    net start SunshineService >nul 2>&1
    if not errorlevel 1 (
        echo Sunshine service restarted.
    ) else (
        echo WARNING: Could not restart Sunshine service.
    )
)
