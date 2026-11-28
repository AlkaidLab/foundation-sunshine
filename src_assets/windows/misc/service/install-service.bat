@echo off
setlocal enabledelayedexpansion
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

rem Exit/result: 0 configured, 1 registration/configuration failed,
rem 2 registered but could not start now (autostart may still be configured).
call :main %*
set "STEP_RC=!ERRORLEVEL!"
> "%~dp0result-service.txt" echo !STEP_RC!
exit /b !STEP_RC!

:main
set "SKIP_START="
for %%A in (%*) do (
    if /I "%%~A"=="--no-start" set "SKIP_START=1"
)

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set SERVICE_NAME=SunshineService
set "SERVICE_BIN=%ROOT_DIR%\tools\sunshinesvc.exe"
set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\LizardByte\Sunshine"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

if not exist "%SERVICE_BIN%" (
    echo ERROR: Service binary not found: "%SERVICE_BIN%"
    exit /b 1
)

rem Set service to demand start. It will be changed to auto later if the user selected that option.
set SERVICE_START_TYPE=demand

rem Remove the legacy SunshineSvc service (NSIS-era name). On clean installs
rem this service obviously does not exist, so both commands return error
rem 1060 / "service name invalid". Silence stdout+stderr — we don't care
rem about the failure, and the noise was confusing users into thinking
rem something was wrong with the new install.
if not exist "%SystemRoot%\System32\sc.exe" (
    echo ERROR: Windows service control tool not found: "%SystemRoot%\System32\sc.exe"
    exit /b 1
)
"%SystemRoot%\System32\net.exe" stop sunshinesvc >nul 2>&1
"%SystemRoot%\System32\sc.exe" delete sunshinesvc >nul 2>&1

rem Decide: reconfigure existing service vs create a new one.
rem `sc config` happily updates binPath in-place, so cross-directory
rem reinstalls don't need a delete+recreate dance.
"%SystemRoot%\System32\sc.exe" qc %SERVICE_NAME% >nul 2>&1
set "SC_QUERY_RC=%ERRORLEVEL%"
if "%SC_QUERY_RC%"=="0" (
    rem Stop first so binPath/start-type changes take effect on next start.
    rem Ignore errors: already-stopped / stop-pending return non-zero.
    "%SystemRoot%\System32\net.exe" stop %SERVICE_NAME% >nul 2>&1
    set SC_CMD=config
) else if "%SC_QUERY_RC%"=="1060" (
    set SC_CMD=create
) else (
    "%SystemRoot%\System32\sc.exe" qc %SERVICE_NAME%
    echo ERROR: Could not query %SERVICE_NAME% ^(exit %SC_QUERY_RC%^).
    exit /b 1
)

rem Restore the user's previous start-type choice if uninstall preserved it.
if exist "%SERVICE_CONFIG_FILE%" (
    for /f "usebackq delims=" %%a in ("%SERVICE_CONFIG_FILE%") do set "SAVED_START_TYPE=%%a"

    if "!SAVED_START_TYPE!"=="2-delayed" (
        set SERVICE_START_TYPE=delayed-auto
    ) else if "!SAVED_START_TYPE!"=="2" (
        set SERVICE_START_TYPE=auto
    ) else if "!SAVED_START_TYPE!"=="3" (
        set SERVICE_START_TYPE=demand
    ) else if "!SAVED_START_TYPE!"=="4" (
        set SERVICE_START_TYPE=disabled
    )
)

echo Setting service start type to: [!SERVICE_START_TYPE!]

rem `sc create` does not accept delayed-auto directly; create as plain auto
rem then upgrade with a second `sc config` below.
set "SC_START_TYPE=!SERVICE_START_TYPE!"
if /I "!SERVICE_START_TYPE!"=="delayed-auto" set "SC_START_TYPE=auto"

rem binPath= MUST embed literal quotes around the path so the registry
rem ImagePath becomes "C:\Program Files\...\sunshinesvc.exe" — both to
rem survive paths with spaces and to close the unquoted-service-path
rem security gap. The `"\"%SERVICE_BIN%\""` form is what produces that:
rem outer "..." is one cmd token; inner \"...\" become real quotes in the
rem argv that sc.exe receives. Triple-quoting (`"""..."""`) splits on
rem internal spaces and makes sc print its usage banner instead.
"%SystemRoot%\System32\sc.exe" !SC_CMD! %SERVICE_NAME% binPath= "\"%SERVICE_BIN%\"" start= !SC_START_TYPE! DisplayName= "Sunshine Service"
if errorlevel 1 (
    echo ERROR: Failed to !SC_CMD! %SERVICE_NAME%.
    exit /b 1
)

if /I "!SERVICE_START_TYPE!"=="delayed-auto" (
    "%SystemRoot%\System32\sc.exe" config %SERVICE_NAME% start= delayed-auto
    if errorlevel 1 (
        echo ERROR: Failed to set delayed auto-start for %SERVICE_NAME%.
        exit /b 1
    )
)

if exist "%SERVICE_CONFIG_FILE%" del "%SERVICE_CONFIG_FILE%" >nul 2>&1

rem Description is metadata only; AV / SCM contention may transiently
rem block it. Never abort install over a missing description string.
"%SystemRoot%\System32\sc.exe" description %SERVICE_NAME% "Sunshine is a self-hosted game stream host for Moonlight." >nul 2>&1

if defined SKIP_START (
    echo %SERVICE_NAME% installed without starting; a pending driver operation must finish first.
    exit /b 0
)

if /I "!SERVICE_START_TYPE!"=="disabled" (
    echo %SERVICE_NAME% installed with disabled start type; not starting.
    exit /b 0
)

rem Start the new service. net start returns non-zero when the service is
rem already running (e.g. SCM auto-started it after sc config), so verify the
rem actual state via sc query before treating that as a failure.
"%SystemRoot%\System32\net.exe" start %SERVICE_NAME%
if errorlevel 1 (
    "%SystemRoot%\System32\sc.exe" query %SERVICE_NAME% | "%SystemRoot%\System32\find.exe" /I "RUNNING" >nul
    if errorlevel 1 (
        echo ERROR: Failed to start %SERVICE_NAME%.
        exit /b 2
    )
)

rem NOTE: we deliberately do NOT wait for the Sunshine HTTPS API to be ready
rem here. `sunshinesvc.exe` is just a wrapper that spawns `sunshine.exe` in
rem the active user session via CreateProcessAsUser and reports RUNNING to
rem SCM immediately. The actual `sunshine.exe` first-run setup (config dir,
rem cert generation, audio/video init, HTTPS bind) takes 10-20s, which used
rem to make the installer's "Installing system service..." page appear
rem stuck for ~30s while we polled localhost:47990. Nothing downstream of
rem this script depends on the API being ready — VerifyServiceInstalled()
rem in sunshine.iss only checks the registry ImagePath, and the finish-page
rem GUI button has its own readiness handling. So just exit fast.

exit /b 0
