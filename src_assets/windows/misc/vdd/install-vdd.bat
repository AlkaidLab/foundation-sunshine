@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem install
set "DRIVER_DIR=%~dp0\driver"
echo %DRIVER_DIR%

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vdd"
set "CONFIG_DIR=%ROOT_DIR%\config"
set "NEFCON=%DIST_DIR%\nefconw.exe"
set "VDD_CONFIG=%CONFIG_DIR%\vdd_settings.xml"

rem First, copy files to target directory so nefconw.exe can be used
if exist "%DIST_DIR%" (
    rmdir /s /q "%DIST_DIR%"
)
mkdir "%DIST_DIR%"
copy "%DRIVER_DIR%\*.*" "%DIST_DIR%"

rem Now we can use nefconw.exe to thoroughly clean up existing VDD adapters
echo Thoroughly cleaning up existing VDD adapters...

rem Remove all device nodes with the same hardware ID (multiple instances)
echo Removing all existing device nodes...
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318
if %ERRORLEVEL% EQU 0 (
    echo Successfully removed device node
) else (
    echo Device node removal failed or not found
)

rem Wait to ensure device is completely removed
timeout /t 3 /nobreak 1>nul

rem Try to uninstall driver completely
echo Uninstalling VDD driver...
"%NEFCON%" --uninstall-driver --inf-path "%DIST_DIR%\ZakoVDD.inf"
if %ERRORLEVEL% EQU 0 (
    echo Successfully uninstalled driver
) else (
    echo Driver uninstall failed or not found
)

rem Wait to ensure driver is completely uninstalled
timeout /t 3 /nobreak 1>nul

rem Clean up registry entries
echo Cleaning registry...
reg delete "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /f 2>nul
if %ERRORLEVEL% EQU 0 (
    echo Successfully cleaned registry
) else (
    echo Registry cleanup failed or not found
)

rem Additional cleanup - remove any remaining device instances
echo Performing additional cleanup...
"%NEFCON%" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318 2>nul
timeout /t 2 /nobreak 1>nul

rem Wait a bit more to ensure everything is cleaned up
timeout /t 5 /nobreak 1>nul

if not exist "%VDD_CONFIG%" (
    copy "%DRIVER_DIR%\vdd_settings.xml" "%VDD_CONFIG%"
)

@REM write registry
reg add "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" /v VDDPATH /t REG_SZ /d "%CONFIG_DIR%" /f

@REM rem install cet
set "CERTIFICATE=%DIST_DIR%\ZakoVDD.cer"
certutil -addstore -f root "%CERTIFICATE%"
@REM certutil -addstore -f TrustedPublisher %CERTIFICATE%

@REM install inf
echo Installing VDD adapter...
"%NEFCON%" --create-device-node --hardware-id Root\ZakoVDD --service-name ZAKO_HDR_FOR_SUNSHINE --class-name Display --class-guid 4D36E968-E325-11CE-BFC1-08002BE10318
"%NEFCON%" --install-driver --inf-path "%DIST_DIR%\ZakoVDD.inf"

echo VDD installation completed!