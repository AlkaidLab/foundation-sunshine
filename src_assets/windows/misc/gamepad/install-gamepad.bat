@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
setlocal enabledelayedexpansion

if /I "%~1"=="--verify-only" (
  set "VERIFY_ONLY=1"
  goto continue
)

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
rem ViGEmBus is pinned, hash-verified at package build time, and bundled next
rem to this script. Installation must not depend on GitHub or a proxy.
set "installer=%~dp0gamepad\ViGEmBus_1.22.0_x64_x86_arm64.exe"
set "expected_sha256=89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A"

if not exist "%installer%" (
  echo ERROR: Bundled ViGEmBus installer not found: "%installer%"
  exit /b 1
)

for /f "usebackq tokens=*" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath $env:installer).Hash"`) do set "actual_sha256=%%H"
if /I not "!actual_sha256!"=="!expected_sha256!" (
  echo ERROR: Bundled ViGEmBus installer failed SHA256 verification.
  echo Expected: !expected_sha256!
  echo Actual:   !actual_sha256!
  exit /b 1
)

if defined VERIFY_ONLY (
  echo Bundled ViGEmBus installer verified successfully.
  exit /b 0
)

rem Install Virtual Gamepad
"%installer%" /passive /norestart
set "INSTALL_RESULT=%ERRORLEVEL%"

exit /b %INSTALL_RESULT%
