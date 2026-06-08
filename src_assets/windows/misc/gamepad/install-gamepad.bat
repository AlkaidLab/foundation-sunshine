@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
setlocal enabledelayedexpansion

rem Optional first argument: "force" to skip the version check and always
rem download + reinstall the latest ViGEmBus release. Without it the script
rem keeps the original behaviour (bail out early if a compatible version
rem is already installed).
if /I "%~1"=="force" (
    echo Force mode: skipping version check, will download and reinstall latest.
    goto continue
)

rem Check if a compatible version of ViGEmBus is already installed (1.17 or later)
powershell -NoProfile -Command "if (Test-Path ($env:SystemRoot + '\System32\drivers\ViGEmBus.sys')) { if ((Get-Item ($env:SystemRoot + '\System32\drivers\ViGEmBus.sys')).VersionInfo.FileVersion -ge [System.Version]'1.17') { exit 2 } }; exit 1"
if %ERRORLEVEL% EQU 2 (
    goto skip
)
goto continue

:skip
echo "The installed version is 1.17 or later, no update needed. Exiting."
exit /b 0

:continue
rem Get temp directory
set "temp_dir=%temp%\Sunshine"

rem Create temp directory if it doesn't exist
if not exist "%temp_dir%" mkdir "%temp_dir%"

rem Get system proxy setting
set proxy= 
for /f "tokens=3" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings" ^| find /i "ProxyEnable"') do (
  set ProxyEnable=%%a
    
  if !ProxyEnable! equ 0x1 (
  for /f "tokens=3" %%a in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings" ^| find /i "ProxyServer"') do (
      set proxy=%%a
      echo Using system proxy !proxy! to download Virtual Gamepad
      set proxy=-x !proxy!
    )
  ) else (
    rem Proxy is not enabled.
  )
)

rem get browser_download_url from asset 0 of https://api.github.com/repos/nefarius/vigembus/releases/latest
set latest_release_url=https://api.github.com/repos/nefarius/vigembus/releases/latest

rem Use curl to get the api response, and find the browser_download_url.
rem `--connect-timeout 10 --max-time 20` ensures we don't hang for minutes if
rem GitHub or the local network is unreachable during install.
for /F "tokens=* USEBACKQ" %%F in (`curl -s --connect-timeout 10 --max-time 20 !proxy! -L %latest_release_url% ^| findstr browser_download_url`) do (
  set browser_download_url=%%F
)

rem Strip quotes
set browser_download_url=%browser_download_url:"=%

rem Remove the browser_download_url key
set browser_download_url=%browser_download_url:browser_download_url: =%

if "%browser_download_url%"=="" (
  echo ERROR: Could not resolve ViGEmBus download URL.
  exit /b 1
)

echo %browser_download_url%

rem Download the exe (with connect/transfer timeout to avoid install hang)
set "installer=%temp_dir%\virtual_gamepad.exe"
curl -f -s -L --connect-timeout 10 --max-time 120 !proxy! -o "%installer%" "%browser_download_url%"
if errorlevel 1 (
  echo Direct download failed, trying mirror...
  curl -f -s -L --connect-timeout 10 --max-time 120 !proxy! -o "%installer%" "https://mirror.ghproxy.com/%browser_download_url%"
)

if not exist "%installer%" (
  echo ERROR: Failed to download ViGEmBus installer.
  exit /b 1
)

for %%I in ("%installer%") do if %%~zI LEQ 0 (
  echo ERROR: Downloaded ViGEmBus installer is empty.
  del /f /q "%installer%" >nul 2>&1
  exit /b 1
)

rem Install Virtual Gamepad
"%installer%" /passive /norestart
set "INSTALL_RESULT=%ERRORLEVEL%"

rem Delete temp directory
rmdir /S /Q "%temp_dir%"

exit /b %INSTALL_RESULT%
