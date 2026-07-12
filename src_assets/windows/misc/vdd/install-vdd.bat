@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul
setlocal enabledelayedexpansion

if /i "%~1"=="--resolve-only" (
    set "RESOLVE_ONLY=1"
)
if /i "%~1"=="--probe-only" (
    set "PROBE_ONLY=1"
)

rem install
set "DRIVER_ROOT=%~dp0\driver"
set "DRIVER_DIR=%DRIVER_ROOT%\win10"
if not exist "%DRIVER_DIR%\ZakoVDD.inf" (
    set "DRIVER_DIR=%DRIVER_ROOT%\latest"
)
set "CONFIG_SOURCE=%DRIVER_ROOT%\vdd_settings.xml"
set "WIN_BUILD="
set "WIN_BUILD_NUM="
set "WIN_BUILD_SOURCE=registry"

if defined VDD_TEST_WIN_BUILD (
    set "WIN_BUILD=%VDD_TEST_WIN_BUILD%"
    set "WIN_BUILD_SOURCE=override"
) else (
    for /f "tokens=3" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber 2^>nul ^| find /i "CurrentBuildNumber"') do set "WIN_BUILD=%%A"
    if not defined WIN_BUILD (
        for /f "tokens=3" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuild 2^>nul ^| find /i "CurrentBuild"') do set "WIN_BUILD=%%A"
    )
)

if defined WIN_BUILD (
    echo(!WIN_BUILD!| findstr /r "^[0-9][0-9]*$" >nul
    if not errorlevel 1 (
        set "WIN_BUILD_NUM=!WIN_BUILD!"
    )
)

if not defined WIN_BUILD (
    echo WARNING: Could not detect Windows build; defaulting to Win10 payload.
)

if defined WIN_BUILD if not defined WIN_BUILD_NUM (
    echo WARNING: Ignoring non-numeric Windows build "!WIN_BUILD!" from !WIN_BUILD_SOURCE!; defaulting to Win10 payload.
)

if defined WIN_BUILD_NUM if !WIN_BUILD_NUM! GEQ 22000 if exist "%DRIVER_ROOT%\latest\ZakoVDD.inf" (
    set "DRIVER_DIR=%DRIVER_ROOT%\latest"
)

if not exist "%DRIVER_DIR%\ZakoVDD.inf" (
    set "DRIVER_DIR=%DRIVER_ROOT%"
)

if exist "%DRIVER_DIR%\vdd_settings.xml" (
    set "CONFIG_SOURCE=%DRIVER_DIR%\vdd_settings.xml"
)

if not exist "%DRIVER_DIR%\ZakoVDD.inf" (
    echo ERROR: VDD driver payload not found in "%DRIVER_DIR%"
    exit /b 1
)

if defined WIN_BUILD_NUM (
    echo Detected Windows build: !WIN_BUILD_NUM!
)
if not defined WIN_BUILD_NUM if defined WIN_BUILD echo Detected Windows build (raw): !WIN_BUILD!
echo Using VDD payload: !DRIVER_DIR!

if defined RESOLVE_ONLY goto :resolve_only

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vdd"
set "CONFIG_DIR=%ROOT_DIR%\config"
set "NEFCON=%ROOT_DIR%\tools\nefconw.exe"
if not exist "%NEFCON%" set "NEFCON=%DIST_DIR%\nefconw.exe"
set "VDD_CONFIG=%CONFIG_DIR%\vdd_settings.xml"

rem Probe the existing device and both driver versions once. Starting a new
rem PowerShell process for every 250 ms poll was itself taking 1-3 seconds.
set "EXISTING_VDD_DEVICES=0"
set "CURRENT_VDD_VERSION="
set "CURRENT_VDD_STATUS="
set "BUNDLED_VDD_VERSION="
for /f "tokens=1,* delims==" %%A in ('powershell -NoProfile -Command ^
    "$device = $null; foreach ($candidate in @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)) { if (@($candidate.HardwareID) -contains 'Root\ZakoVDD') { $device = $candidate; break } };" ^
    "$driverVerLine = (Select-String -LiteralPath (Join-Path $env:DRIVER_DIR 'ZakoVDD.inf') -Pattern '^\s*DriverVer\s*=' -ErrorAction SilentlyContinue).Line;" ^
    "$bundled = if ($driverVerLine) { ($driverVerLine -split ',')[-1].Trim() } else { '' };" ^
    "$current = if ($device) { (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data } else { '' };" ^
    "$status = if ($device) { $device.Status } else { '' };" ^
    "Write-Output ('EXISTING_VDD_DEVICES=' + @($device).Count); Write-Output ('CURRENT_VDD_VERSION=' + $current); Write-Output ('CURRENT_VDD_STATUS=' + $status); Write-Output ('BUNDLED_VDD_VERSION=' + $bundled)"') do set "%%A=%%B"

echo Existing VDD devices: !EXISTING_VDD_DEVICES!
if defined CURRENT_VDD_VERSION echo Installed VDD version: !CURRENT_VDD_VERSION!
if defined CURRENT_VDD_STATUS echo Installed VDD status: !CURRENT_VDD_STATUS!
if defined BUNDLED_VDD_VERSION echo Bundled VDD version: !BUNDLED_VDD_VERSION!
if defined PROBE_ONLY exit /b 0

set "VDD_CLEANUP_REQUIRED=0"
if !EXISTING_VDD_DEVICES! GTR 0 set "VDD_CLEANUP_REQUIRED=1"
set "VDD_INSTALL_REQUIRED=1"
if /i "!CURRENT_VDD_STATUS!"=="OK" if defined CURRENT_VDD_VERSION if defined BUNDLED_VDD_VERSION if /i "!CURRENT_VDD_VERSION!"=="!BUNDLED_VDD_VERSION!" (
    set "VDD_CLEANUP_REQUIRED=0"
    set "VDD_INSTALL_REQUIRED=0"
    echo Matching VDD driver is already active; skipping driver reinstall.
)

rem First, copy files to target directory so nefconw.exe can be used
if exist "%DIST_DIR%" (
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"
copy /y "%DRIVER_DIR%\*.*" "%DIST_DIR%" >nul

if "!VDD_CLEANUP_REQUIRED!"=="1" goto :cleanup_existing_vdd
echo No existing VDD device requires cleanup.
goto :after_existing_vdd_cleanup

:cleanup_existing_vdd
echo Existing VDD installation detected; cleaning it up...

rem Remove all device nodes with the same hardware ID (multiple instances)
echo Removing all existing device nodes...
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318
if %ERRORLEVEL% EQU 0 (
    echo Successfully removed device node
) else (
    echo Device node removal failed or not found
)

rem One PowerShell process performs the whole bounded wait.
call :wait_for_vdd_removal 10
set "VDD_REMOVE_WAIT_RESULT=!ERRORLEVEL!"

rem Clean up registry entries
echo Cleaning registry...
reg delete "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /f 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Successfully cleaned registry
) else (
    echo Registry cleanup failed or not found
)

if not "!VDD_REMOVE_WAIT_RESULT!"=="0" (
    echo Device still present; performing one fallback removal...
    "%NEFCON%" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318 2>nul
    call :wait_for_vdd_removal 10
)

:after_existing_vdd_cleanup

if not exist "%CONFIG_DIR%" mkdir "%CONFIG_DIR%"
if not exist "%VDD_CONFIG%" (
    copy /y "%CONFIG_SOURCE%" "%VDD_CONFIG%" >nul
)

@REM write registry
reg add "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /v VDDPATH /t REG_SZ /d "%CONFIG_DIR%" /f

if "!VDD_INSTALL_REQUIRED!"=="0" goto :vdd_install_complete

@REM rem install cet
set "CERTIFICATE=%DIST_DIR%\ZakoVDD.cer"
certutil -addstore -f root "%CERTIFICATE%"
@REM certutil -addstore -f TrustedPublisher %CERTIFICATE%

rem Stage the package before creating the root device. The old order created an
rem unbound ROOT\DISPLAY node and then updated it, which made SetupAPI remove and
rem restart that brand-new device tree and could block for about 60 seconds.
echo [%TIME%] Staging VDD driver package...
pnputil /add-driver "%DIST_DIR%\ZakoVDD.inf"
if errorlevel 1 (
    echo ERROR: Failed to stage the VDD driver package.
    exit /b 1
)

echo [%TIME%] Creating VDD adapter...
"%NEFCON%" --create-device-node --hardware-id Root\ZakoVDD --service-name ZAKO_HDR_FOR_SUNSHINE --class-name Display --class-guid 4D36E968-E325-11CE-BFC1-08002BE10318
if errorlevel 1 (
    echo ERROR: Failed to create the VDD device node.
    exit /b 1
)

rem Creating the root node does not reliably trigger automatic binding. The
rem package is already staged, so the explicit install completes without the
rem expensive remove-and-restart cycle seen in the old order.
"%NEFCON%" --install-driver --inf-path "%DIST_DIR%\ZakoVDD.inf"
if errorlevel 1 (
    echo ERROR: Explicit VDD driver installation failed.
    exit /b 1
)
call :wait_for_vdd_ready 10
if errorlevel 1 (
    echo ERROR: VDD device did not become ready with version !BUNDLED_VDD_VERSION!.
    exit /b 1
)

:vdd_install_complete
echo [%TIME%] VDD adapter is ready.
echo VDD installation completed!
goto :eof

:resolve_only
echo RESOLVED_WIN_BUILD=!WIN_BUILD!
echo RESOLVED_WIN_BUILD_NUM=!WIN_BUILD_NUM!
echo RESOLVED_DRIVER_DIR=!DRIVER_DIR!
echo RESOLVED_CONFIG_SOURCE=!CONFIG_SOURCE!
exit /b 0

:wait_for_vdd_removal
powershell -NoProfile -Command "$deadline = [DateTime]::UtcNow.AddSeconds(%~1); do { $device = $null; foreach ($candidate in @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)) { if (@($candidate.HardwareID) -contains 'Root\ZakoVDD') { $device = $candidate; break } }; if (-not $device) { exit 0 }; Start-Sleep -Milliseconds 250 } while ([DateTime]::UtcNow -lt $deadline); exit 1"
exit /b %ERRORLEVEL%

:wait_for_vdd_ready
powershell -NoProfile -Command "$deadline = [DateTime]::UtcNow.AddSeconds(%~1); do { $device = $null; foreach ($candidate in @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)) { if (@($candidate.HardwareID) -contains 'Root\ZakoVDD') { $device = $candidate; break } }; if ($device -and ($device.Status -eq 'OK')) { $version = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data; if (-not $env:BUNDLED_VDD_VERSION -or ($version -eq $env:BUNDLED_VDD_VERSION)) { exit 0 } }; Start-Sleep -Milliseconds 250 } while ([DateTime]::UtcNow -lt $deadline); exit 1"
exit /b %ERRORLEVEL%
