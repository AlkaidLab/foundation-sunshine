@echo off
:: Check for administrator privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Please run this script as administrator
    pause
    exit /b
)

:: Check if VB-Cable driver is already installed
reg query "HKLM\SOFTWARE\VB-Audio" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable driver is already installed
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
