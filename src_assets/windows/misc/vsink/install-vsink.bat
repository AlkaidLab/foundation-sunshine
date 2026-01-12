@echo off
:: Check for administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Please run this script as administrator
    pause
    exit /b
)

:: Check if VB-Cable driver is already installed
:: Use PowerShell to check registry (more reliable, doesn't depend on PATH)
powershell -Command "try { $key = Get-ItemProperty -Path 'HKLM:\SOFTWARE\VB-Audio\Cable' -ErrorAction SilentlyContinue; if ($key) { exit 0 } else { exit 1 } } catch { exit 1 }" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed (detected via registry)
    pause
    exit /b
)

:: Method 2: Check if VB-Cable audio device exists in system (fallback method)
:: This method is more reliable as it checks actual hardware devices
powershell -Command "try { $devices = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object {$_.FriendlyName -like '*VB-Cable*' -or $_.FriendlyName -like '*CABLE*' -or $_.FriendlyName -like '*VB-Audio Virtual Cable*'}; if ($devices) { exit 0 } else { exit 1 } } catch { exit 1 }" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed (detected via audio device)
    pause
    exit /b
)

:: Method 3: Check audio devices via registry using PowerShell (additional fallback)
powershell -Command "try { $devices = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\MMDEVAPI\*' -ErrorAction SilentlyContinue | Where-Object {$_.FriendlyName -like '*CABLE*' -or $_.FriendlyName -like '*VB-Cable*'}; if ($devices) { exit 0 } else { exit 1 } } catch { exit 1 }" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed (detected via device registry)
    pause
    exit /b
)

:: Set variables
set "installer=VBCABLE_Driver_Pack43.zip"
set "download_url=https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip"
set "temp_dir=%TEMP%\vb_cable_install"

:: Create temp directory
if not exist "%temp_dir%" mkdir "%temp_dir%"

:: Download installer
echo Downloading VB-Cable driver...
powershell -Command "Invoke-WebRequest -Uri '%download_url%' -OutFile '%temp_dir%\%installer%'"

:: Extract files
echo Extracting files...
powershell -Command "Expand-Archive -Path '%temp_dir%\%installer%' -DestinationPath '%temp_dir%' -Force"

:: Install driver
echo Installing VB-Cable driver...
start /wait "" "%temp_dir%\VBCABLE_Setup_x64.exe" /S

:: Clean up temp files
echo Cleaning up temporary files...
rd /s /q "%temp_dir%"

echo VB-Cable driver installation completed!
pause
