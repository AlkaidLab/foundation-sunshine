@echo off
setlocal DisableDelayedExpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"
:: Check for administrator privileges
"%SystemRoot%\System32\net.exe" session >nul 2>&1
if %errorLevel% neq 0 (
    echo Please run this script as administrator
    pause
    exit /b 1
)

:: Registry entries can survive driver removal; verify the actual driver.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "try { $devices = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop | Where-Object { $_.DeviceName -eq 'VB-Audio Virtual Cable' -and $_.DriverProviderName -eq 'VB-Audio Software' }); if ($devices.Count) { exit 0 } else { exit 1 } } catch { exit 1 }" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed
    pause
    exit /b 0
)

:: Fall back to the audio endpoints if the driver inventory is unavailable.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "try { $devices = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object {$_.FriendlyName -like '*VB-Cable*' -or $_.FriendlyName -like '*VB-Audio Virtual Cable*'}; if ($devices) { exit 0 } else { exit 1 } } catch { exit 1 }" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed - detected via audio device
    pause
    exit /b 0
)

:: Set variables
set "installer=VBCABLE_Driver_Pack43.zip"
set "download_url=https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip"
set "temp_dir=%TEMP%\vb_cable_install"

:: Create temp directory
if not exist "%temp_dir%" mkdir "%temp_dir%"
if not exist "%temp_dir%" (
    echo ERROR: Could not create the temporary directory.
    pause
    exit /b 1
)

:: Download installer
echo Downloading VB-Cable driver...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; Invoke-WebRequest -UseBasicParsing -Uri $env:download_url -OutFile (Join-Path $env:temp_dir $env:installer)"
if errorlevel 1 (
    echo ERROR: Could not download the VB-Cable driver.
    pause
    exit /b 1
)

:: Extract files
echo Extracting files...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; Expand-Archive -LiteralPath (Join-Path $env:temp_dir $env:installer) -DestinationPath $env:temp_dir -Force"
if errorlevel 1 (
    echo ERROR: Could not extract the VB-Cable driver.
    pause
    exit /b 1
)

:: Install driver
echo Installing VB-Cable driver...
if not exist "%temp_dir%\VBCABLE_Setup_x64.exe" (
    echo ERROR: The VB-Cable setup program is missing.
    pause
    exit /b 1
)
start /wait "" "%temp_dir%\VBCABLE_Setup_x64.exe" /S
set "SETUP_RC=%ERRORLEVEL%"
if not "%SETUP_RC%"=="0" if not "%SETUP_RC%"=="3010" (
    echo ERROR: The VB-Cable setup program failed.
    pause
    exit /b 1
)

:: Clean up temp files
echo Cleaning up temporary files...
rd /s /q "%temp_dir%"

if "%SETUP_RC%"=="3010" (
    echo VB-Cable installation requires a restart.
    pause
    exit /b 3010
)
echo VB-Cable driver installation completed!
pause
exit /b 0
