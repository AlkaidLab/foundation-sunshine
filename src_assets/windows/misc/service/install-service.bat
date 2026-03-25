@echo off
setlocal enabledelayedexpansion

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set SERVICE_NAME=SunshineService
set "SERVICE_BIN=%ROOT_DIR%\tools\sunshinesvc.exe"
set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\LizardByte\Sunshine"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

rem Set service to demand start. It will be changed to auto later if the user selected that option.
set SERVICE_START_TYPE=demand

rem Remove the legacy SunshineSvc service
net stop sunshinesvc
sc delete sunshinesvc

rem Check if SunshineService already exists
sc qc %SERVICE_NAME% > nul 2>&1
if %ERRORLEVEL%==0 (
    rem Stop the existing service if running
    net stop %SERVICE_NAME%

    rem Reconfigure the existing service
    set SC_CMD=config
) else (
    rem Create a new service
    set SC_CMD=create
)

rem Check if we have a saved start type from previous installation
if exist "%SERVICE_CONFIG_FILE%" (
    rem Debug output file content
    type "%SERVICE_CONFIG_FILE%"

    rem Read the saved start type
    for /f "usebackq delims=" %%a in ("%SERVICE_CONFIG_FILE%") do (
        set "SAVED_START_TYPE=%%a"
    )

    echo Raw saved start type: [!SAVED_START_TYPE!]

    rem Check start type
    if "!SAVED_START_TYPE!"=="2-delayed" (
        set SERVICE_START_TYPE=delayed-auto
    ) else if "!SAVED_START_TYPE!"=="2" (
        set SERVICE_START_TYPE=auto
    ) else if "!SAVED_START_TYPE!"=="3" (
        set SERVICE_START_TYPE=demand
    ) else if "!SAVED_START_TYPE!"=="4" (
        set SERVICE_START_TYPE=disabled
    )

    del "%SERVICE_CONFIG_FILE%"
)

echo Setting service start type set to: [!SERVICE_START_TYPE!]

rem Run the sc command to create/reconfigure the service
sc %SC_CMD% %SERVICE_NAME% binPath= "%SERVICE_BIN%" start= %SERVICE_START_TYPE% DisplayName= "Sunshine Service"

rem Set the description of the service
sc description %SERVICE_NAME% "Sunshine is a self-hosted game stream host for Moonlight."

rem Start the new service
net start %SERVICE_NAME%

rem Determine the Web UI port from config (default base port 47989 + 1 = 47990)
set /a WEB_PORT=47990
set "SUNSHINE_CONF=%ROOT_DIR%\config\sunshine.conf"
if exist "%SUNSHINE_CONF%" (
    for /f "usebackq tokens=1,* delims==" %%a in ("%SUNSHINE_CONF%") do (
        set "KEY=%%a"
        set "VAL=%%b"
        rem Trim spaces from key
        for /f "tokens=* delims= " %%k in ("!KEY!") do set "KEY=%%k"
        if "!KEY!"=="port" (
            for /f "tokens=* delims= " %%v in ("!VAL!") do set "VAL=%%v"
            set /a WEB_PORT=!VAL!+1
        )
    )
)

rem Wait for Sunshine API to be ready
echo Waiting for Sunshine API on port !WEB_PORT!...
set /a WAIT_COUNT=0
set /a WAIT_MAX=15
:wait_loop
if !WAIT_COUNT! GEQ !WAIT_MAX! (
    echo Sunshine API did not become ready within %WAIT_MAX% seconds, continuing anyway...
    goto :wait_done
)
powershell -NoProfile -Command "try { $c = [System.Net.Sockets.TcpClient]::new(); $c.Connect('127.0.0.1', !WEB_PORT!); $c.Close(); exit 0 } catch { exit 1 }" >nul 2>&1
if !ERRORLEVEL!==0 (
    echo Sunshine API is ready on port !WEB_PORT!.
    goto :wait_done
)
set /a WAIT_COUNT+=1
timeout /t 1 /nobreak >nul
goto :wait_loop
:wait_done
