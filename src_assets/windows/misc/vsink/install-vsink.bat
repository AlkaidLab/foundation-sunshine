@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
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

:: Use an unpredictable temp directory under the admin user's %TEMP% to defeat
:: any pre-positioned binary at a guessable path. %TEMP% under an elevated
:: shell is the admin's own profile (not world-writable), but using a fresh
:: random directory eliminates the residual TOCTOU window between mkdir and
:: VBCABLE_Setup_x64.exe launch.
for /F "usebackq delims=" %%R in (`powershell -NoProfile -Command "[guid]::NewGuid().ToString('N')"`) do set "RAND_ID=%%R"
if "%RAND_ID%"=="" set "RAND_ID=%RANDOM%%RANDOM%%RANDOM%"
set "temp_dir=%TEMP%\sunshine-vbcable-%RAND_ID%"

:: Create temp directory (start clean; refuse to proceed if pre-existing path
:: cannot be removed, in case an attacker pre-created a hardlink/junction).
if exist "%temp_dir%" rd /s /q "%temp_dir%"
if exist "%temp_dir%" (
    echo ERROR: Could not prepare temp directory: %temp_dir%
    pause
    exit /b 1
)
mkdir "%temp_dir%"

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
