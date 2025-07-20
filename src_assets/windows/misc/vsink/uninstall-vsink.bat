@echo off
:: Uninstall VB-Cable virtual audio device
:: Requires administrator privileges

:: Check if running as administrator
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Please run this script as administrator
    pause
    exit /b
)

:: Stop and remove VB-Cable service
echo Stopping VB-Cable service...
net stop "VB-Audio Virtual Cable" >nul 2>&1

echo Removing VB-Cable driver...
pnputil /delete-driver oem*.inf /uninstall /force >nul 2>&1

:: Clean registry entries
echo Cleaning registry...
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\VB-Audio Virtual Cable" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\VB-Audio" /f >nul 2>&1

:: Remove program files
echo Removing program files...
rd /s /q "%ProgramFiles%\VB\Cable" >nul 2>&1
rd /s /q "%ProgramFiles(x86)%\VB\Cable" >nul 2>&1

echo Uninstallation complete!
echo Please restart your computer for changes to take effect
pause
