@echo off
setlocal enabledelayedexpansion

set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul

set "VDD_HARDWARE_ID=Root\ZakoVDD"
set "VDD_CLASS_GUID=4D36E968-E325-11CE-BFC1-08002BE10318"
set "VDD_SERVICE_NAME=ZAKO_HDR_FOR_SUNSHINE"

for %%A in (%*) do (
    if /i "%%~A"=="--resolve-only" set "RESOLVE_ONLY=1"
    if /i "%%~A"=="--probe-only" set "PROBE_ONLY=1"
)
call :select_payload || exit /b 1
call :print_payload

if defined RESOLVE_ONLY (
    call :print_resolution
    exit /b 0
)

call :init_paths
call :probe_vdd || exit /b 1
call :choose_action
if defined PROBE_ONLY exit /b 0

call :stage_payload || exit /b 1
if "!VDD_CLEANUP_REQUIRED!"=="1" (
    call :remove_vdd || exit /b 1
)
call :configure_vdd || exit /b 1
if "!VDD_INSTALL_REQUIRED!"=="1" (
    call :install_vdd || exit /b 1
)

echo [%TIME%] VDD adapter is ready.
echo VDD installation completed!
exit /b 0

:select_payload
set "DRIVER_ROOT=%~dp0driver"
set "DRIVER_DIR=%DRIVER_ROOT%\win10"
set "CONFIG_SOURCE=%DRIVER_ROOT%\vdd_settings.xml"
set "WIN_BUILD="
set "WIN_BUILD_NUM="
set "WIN_BUILD_SOURCE=registry"

if defined VDD_TEST_WIN_BUILD (
    set "WIN_BUILD=!VDD_TEST_WIN_BUILD!"
    set "WIN_BUILD_SOURCE=override"
) else (
    for /f "tokens=3" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber 2^>nul ^| find /i "CurrentBuildNumber"') do set "WIN_BUILD=%%A"
    if not defined WIN_BUILD (
        for /f "tokens=3" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuild 2^>nul ^| find /i "CurrentBuild"') do set "WIN_BUILD=%%A"
    )
)

if defined WIN_BUILD (
    echo(!WIN_BUILD!| findstr /r "^[0-9][0-9]*$" >nul
    if not errorlevel 1 set "WIN_BUILD_NUM=!WIN_BUILD!"
)

if not defined WIN_BUILD echo WARNING: Could not detect Windows build; defaulting to Win10 payload.
if defined WIN_BUILD if not defined WIN_BUILD_NUM echo WARNING: Ignoring non-numeric Windows build "!WIN_BUILD!" from !WIN_BUILD_SOURCE!; defaulting to Win10 payload.

if not exist "!DRIVER_DIR!\ZakoVDD.inf" set "DRIVER_DIR=!DRIVER_ROOT!\latest"
if defined WIN_BUILD_NUM if !WIN_BUILD_NUM! GEQ 22000 if exist "!DRIVER_ROOT!\latest\ZakoVDD.inf" set "DRIVER_DIR=!DRIVER_ROOT!\latest"
if not exist "!DRIVER_DIR!\ZakoVDD.inf" set "DRIVER_DIR=!DRIVER_ROOT!"
if exist "!DRIVER_DIR!\vdd_settings.xml" set "CONFIG_SOURCE=!DRIVER_DIR!\vdd_settings.xml"

if not exist "!DRIVER_DIR!\ZakoVDD.inf" (
    echo ERROR: VDD driver payload not found in "!DRIVER_DIR!"
    exit /b 1
)
if not exist "!CONFIG_SOURCE!" (
    echo ERROR: VDD configuration template not found at "!CONFIG_SOURCE!"
    exit /b 1
)
exit /b 0

:print_payload
if defined WIN_BUILD_NUM echo Detected Windows build: !WIN_BUILD_NUM!
if not defined WIN_BUILD_NUM if defined WIN_BUILD echo Detected Windows build ^(raw^): !WIN_BUILD!
echo Using VDD payload: !DRIVER_DIR!
exit /b 0

:print_resolution
echo RESOLVED_WIN_BUILD=!WIN_BUILD!
echo RESOLVED_WIN_BUILD_NUM=!WIN_BUILD_NUM!
echo RESOLVED_DRIVER_DIR=!DRIVER_DIR!
echo RESOLVED_CONFIG_SOURCE=!CONFIG_SOURCE!
exit /b 0

:init_paths
for %%I in ("%~dp0..") do set "ROOT_DIR=%%~fI"
set "DIST_DIR=!ROOT_DIR!\tools\vdd"
set "CONFIG_DIR=!ROOT_DIR!\config"
set "VDD_CONFIG=!CONFIG_DIR!\vdd_settings.xml"
set "VDD_HELPER=%~dp0vdd-device-helper.ps1"
set "NEFCON=!ROOT_DIR!\tools\nefconw.exe"
if not exist "!NEFCON!" set "NEFCON=!DIST_DIR!\nefconw.exe"
exit /b 0

:probe_vdd
if not exist "!VDD_HELPER!" (
    echo ERROR: VDD device helper not found at "!VDD_HELPER!"
    exit /b 1
)

set "VDD_DEVICE_PRESENT=0"
set "CURRENT_VDD_VERSION="
set "CURRENT_VDD_STATUS="
set "BUNDLED_VDD_VERSION="
for /f "tokens=1,* delims==" %%A in ('powershell -NoProfile -ExecutionPolicy Bypass -File "!VDD_HELPER!" -Action Probe -InfPath "!DRIVER_DIR!\ZakoVDD.inf"') do set "%%A=%%B"
if not defined VDD_PROBE_OK (
    echo ERROR: Failed to inspect the VDD device and driver versions.
    exit /b 1
)

echo Existing VDD device: !VDD_DEVICE_PRESENT!
if defined CURRENT_VDD_VERSION echo Installed VDD version: !CURRENT_VDD_VERSION!
if defined CURRENT_VDD_STATUS echo Installed VDD status: !CURRENT_VDD_STATUS!
echo Bundled VDD version: !BUNDLED_VDD_VERSION!
exit /b 0

:choose_action
set "VDD_CLEANUP_REQUIRED=!VDD_DEVICE_PRESENT!"
set "VDD_INSTALL_REQUIRED=1"

if not "!CURRENT_VDD_STATUS!"=="OK" exit /b 0
if not defined CURRENT_VDD_VERSION exit /b 0
if not defined BUNDLED_VDD_VERSION exit /b 0
if /i not "!CURRENT_VDD_VERSION!"=="!BUNDLED_VDD_VERSION!" exit /b 0

set "VDD_CLEANUP_REQUIRED=0"
set "VDD_INSTALL_REQUIRED=0"
echo Matching VDD driver is already active; skipping driver reinstall.
exit /b 0

:stage_payload
if exist "!DIST_DIR!" rmdir /s /q "!DIST_DIR!"
mkdir "!DIST_DIR!" 2>nul
if errorlevel 1 (
    echo ERROR: Failed to create VDD payload directory "!DIST_DIR!".
    exit /b 1
)
copy /y "!DRIVER_DIR!\*.*" "!DIST_DIR!" >nul
if errorlevel 1 (
    echo ERROR: Failed to stage the VDD payload.
    exit /b 1
)
exit /b 0

:remove_vdd
echo Existing VDD installation detected; removing its device node...
"!NEFCON!" --remove-device-node --hardware-id !VDD_HARDWARE_ID! --class-guid !VDD_CLASS_GUID!
call :wait_vdd Removed 10
if not errorlevel 1 goto :vdd_removed

echo Device still present; retrying removal once...
"!NEFCON!" --remove-device-node --hardware-id !VDD_HARDWARE_ID! --class-guid !VDD_CLASS_GUID! 2>nul
call :wait_vdd Removed 10
if errorlevel 1 (
    echo ERROR: VDD device remained present after removal.
    exit /b 1
)

:vdd_removed
reg delete "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /f >nul 2>&1
exit /b 0

:configure_vdd
if not exist "!CONFIG_DIR!" mkdir "!CONFIG_DIR!" 2>nul
if not exist "!CONFIG_DIR!" (
    echo ERROR: Failed to create VDD configuration directory "!CONFIG_DIR!".
    exit /b 1
)

if not exist "!VDD_CONFIG!" copy /y "!CONFIG_SOURCE!" "!VDD_CONFIG!" >nul
if not exist "!VDD_CONFIG!" (
    echo ERROR: Failed to create VDD configuration file "!VDD_CONFIG!".
    exit /b 1
)

reg add "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /v VDDPATH /t REG_SZ /d "!CONFIG_DIR!" /f >nul
if errorlevel 1 (
    echo ERROR: Failed to register the VDD configuration path.
    exit /b 1
)
exit /b 0

:install_vdd
set "CERTIFICATE=!DIST_DIR!\ZakoVDD.cer"
certutil -addstore -f root "!CERTIFICATE!" >nul
if errorlevel 1 (
    echo ERROR: Failed to import the VDD certificate.
    exit /b 1
)

echo [%TIME%] Staging VDD driver package...
pnputil /add-driver "!DIST_DIR!\ZakoVDD.inf"
set "PNPUTIL_EXIT_CODE=!ERRORLEVEL!"
if not "!PNPUTIL_EXIT_CODE!"=="0" if not "!PNPUTIL_EXIT_CODE!"=="3010" (
    echo ERROR: Failed to stage the VDD driver package ^(exit code !PNPUTIL_EXIT_CODE!^).
    exit /b 1
)
if "!PNPUTIL_EXIT_CODE!"=="3010" echo VDD driver package staged; Windows reports that a reboot is required.

echo [%TIME%] Creating VDD adapter...
"!NEFCON!" --create-device-node --hardware-id !VDD_HARDWARE_ID! --service-name !VDD_SERVICE_NAME! --class-name Display --class-guid !VDD_CLASS_GUID!
if errorlevel 1 (
    echo ERROR: Failed to create the VDD device node.
    exit /b 1
)

"!NEFCON!" --install-driver --inf-path "!DIST_DIR!\ZakoVDD.inf"
if errorlevel 1 (
    echo ERROR: Failed to bind the VDD driver.
    exit /b 1
)

call :wait_vdd Ready 10
if errorlevel 1 (
    echo ERROR: VDD device did not become ready with version !BUNDLED_VDD_VERSION!.
    exit /b 1
)
exit /b 0

:wait_vdd
powershell -NoProfile -ExecutionPolicy Bypass -File "!VDD_HELPER!" -Action %~1 -ExpectedVersion "!BUNDLED_VDD_VERSION!" -TimeoutSeconds %~2
exit /b !ERRORLEVEL!
