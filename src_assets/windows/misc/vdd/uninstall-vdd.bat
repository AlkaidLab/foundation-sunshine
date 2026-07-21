@echo off
setlocal enabledelayedexpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
chcp 65001 >nul

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "DIST_DIR=%ROOT_DIR%\tools\vdd"
set "NEFCON=%ROOT_DIR%\tools\nefconw.exe"
if not exist "%NEFCON%" set "NEFCON=%DIST_DIR%\nefconw.exe"
set "VDD_HELPER=%~dp0vdd-device-helper.ps1"
set "UNINSTALL_RESULT=0"

if exist "!VDD_HELPER!" (
    echo Removing all VDD device nodes...
    powershell -NoProfile -ExecutionPolicy Bypass -File "!VDD_HELPER!" -Action Remove -NefconPath "!NEFCON!" -TimeoutSeconds 30
    if errorlevel 1 (
        echo WARNING: Failed to remove every VDD device node; driver packages will be preserved.
        set "UNINSTALL_RESULT=1"
    ) else (
        echo Removing all VDD driver packages...
        powershell -NoProfile -ExecutionPolicy Bypass -File "!VDD_HELPER!" -Action CleanupPackages
        if errorlevel 1 (
            echo WARNING: Failed to remove every VDD driver package.
            set "UNINSTALL_RESULT=1"
        )
    )
    goto :cleanup
)

rem Compatibility fallback for an installation that predates the helper.
if not exist "!NEFCON!" (
    echo WARNING: VDD helper and nefconw.exe are unavailable; skipping driver removal.
    set "UNINSTALL_RESULT=1"
    goto :cleanup
)

echo Removing VDD device node...
"!NEFCON!" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318
timeout /t 1 /nobreak >nul 2>&1

if exist "!DIST_DIR!\ZakoVDD.inf" (
    echo Uninstalling VDD driver package...
    "!NEFCON!" --uninstall-driver --inf-path "!DIST_DIR!\ZakoVDD.inf"
) else (
    echo WARNING: ZakoVDD.inf not found in "!DIST_DIR!", skipping driver package uninstall.
    set "UNINSTALL_RESULT=1"
)

"!NEFCON!" --remove-device-node --hardware-id Root\ZakoVDD --class-guid 4d36e968-e325-11ce-bfc1-08002be10318 >nul 2>&1

:cleanup
echo Cleaning registry...
reg delete "HKLM\SOFTWARE\ZakoTech" /f 2>nul
if exist "!DIST_DIR!" (
    rmdir /S /Q "!DIST_DIR!"
)
echo VDD uninstall completed.
exit /b !UNINSTALL_RESULT!
