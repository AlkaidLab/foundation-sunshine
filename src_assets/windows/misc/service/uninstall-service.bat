@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
setlocal enabledelayedexpansion

set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\LizardByte\Sunshine"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

rem Save the current service start type to a file if the service exists
sc qc SunshineService >nul 2>&1
if %ERRORLEVEL%==0 (
    if not exist "%SERVICE_CONFIG_DIR%\" mkdir "%SERVICE_CONFIG_DIR%\"

    rem Get the start type
    for /f "tokens=3" %%i in ('sc qc SunshineService ^| findstr /C:"START_TYPE"') do (
        set "CURRENT_START_TYPE=%%i"
    )

    rem Set the content to write
    if "!CURRENT_START_TYPE!"=="2" (
        sc qc SunshineService | findstr /C:"(DELAYED)" >nul
        if !ERRORLEVEL!==0 (
            set "CONTENT=2-delayed"
        ) else (
            set "CONTENT=2"
        )
    ) else if "!CURRENT_START_TYPE!" NEQ "" (
        set "CONTENT=!CURRENT_START_TYPE!"
    ) else (
        set "CONTENT=unknown"
    )

    rem Write content to file
    echo !CONTENT!> "%SERVICE_CONFIG_FILE%"
)

rem Stop and delete the legacy SunshineSvc service (non-blocking)
sc stop sunshinesvc >nul 2>&1
sc delete sunshinesvc >nul 2>&1

rem Force-kill the service binary FIRST so SCM can transition the service to
rem STOPPED quickly. We deliberately avoid `net stop`, which blocks for up to
rem 30 seconds waiting on the service's stop handler — that is the typical
rem cause of "Inno uninstaller appears frozen".
taskkill /f /im sunshinesvc.exe >nul 2>&1

rem Issue a stop control as a courtesy (idempotent, returns quickly), then
rem delete. If still in stop-pending, SCM marks the service for deletion and
rem removes it once stopped — which is immediate after the taskkill above.
sc stop SunshineService >nul 2>&1
sc delete SunshineService >nul 2>&1
exit /b 0
