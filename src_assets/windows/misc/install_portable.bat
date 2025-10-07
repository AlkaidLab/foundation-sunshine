@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
color 0F

REM ================================================
REM Initialize environment variables
REM ================================================

REM Set Sunshine root directory path (current directory)
set "SUNSHINE_ROOT=%~dp0"
REM Get script directory path (scripts folder)
set "SCRIPT_DIR=%~dp0scripts"
REM Set language file directory path
set "LANG_DIR=%SCRIPT_DIR%\languages"

REM ================================================
REM Multi-language support initialization
REM ================================================

REM Detect system language and select corresponding language file
call :DetectLanguage
REM Load selected language file
call :LoadLanguageFile "%SELECTED_LANG%"

REM ================================================
REM Display program title
REM ================================================

echo.
echo ================================================
echo           %TITLE%
echo ================================================
echo.

REM ================================================
REM Permission check and elevation
REM ================================================

REM Check if running with administrator privileges
net session >nul 2>&1
if !errorLevel! neq 0 (
    call :LogError "%ERROR_ADMIN%"
    echo %ERROR_ADMIN_DESC%
    echo.
    echo %ADMIN_ELEVATE_PROMPT%
    set /p elevate_choice="%ADMIN_ELEVATE_CONFIRM% "
    if /i "!elevate_choice!"=="Y" (
        echo %ADMIN_ELEVATING%...
        REM Use PowerShell to elevate and restart script
        powershell -NoProfile -Command "Start-Process -FilePath $env:ComSpec -ArgumentList '/k','\"%~f0\"' -Verb RunAs -WorkingDirectory '%~dp0'"
        pause
        exit /b 0
    ) else (
        echo %ADMIN_ELEVATE_CANCEL%
        echo.
        pause
        exit /b 1
    )
)

call :LogInfo "%INFO_ADMIN%"
echo.

REM ================================================
REM Environment validation
REM ================================================

REM Verify Sunshine executable file exists
if not exist "!SUNSHINE_ROOT!sunshine.exe" (
    call :LogError "%ERROR_EXE%"
    echo %ERROR_EXE_DESC%
    echo Expected path: !SUNSHINE_ROOT!sunshine.exe
    echo.
    pause
    exit /b 1
)

call :LogInfo "%INFO_EXE%: !SUNSHINE_ROOT!sunshine.exe"
echo.

REM ================================================
REM User confirmation
REM ================================================

REM Display list of operations to be performed and wait for user confirmation
call :ShowScriptConfirmation
if "!CONFIRM_RESULT!" neq "1" (
    echo %SCRIPT_CONFIRM_CANCEL%
    echo.
    goto :SkipInstallation
)

echo %SCRIPT_CONFIRM_PROCEEDING%
echo.

echo.
echo ================================================
echo           %TITLE_INSTALL%
echo ================================================
echo.

REM ================================================
REM Component installation process
REM ================================================

REM Step 1 - Install virtual display driver (VDD)
call :LogStep "1/4" "%STEP_VDD%..."
REM Check if virtual display is already installed
reg query "HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter" >nul 2>&1
if !errorLevel! neq 0 (
    set "vdd_choice=Y"
    set /p vdd_choice="%VDD_PROMPT% "
    if /i "!vdd_choice!"=="Y" (
        call :LogInfo "%INFO_VDD_INSTALL%..."
        REM Call virtual display installation script
        call "%SCRIPT_DIR%\install-vdd.bat"
        if !errorLevel! equ 0 (
            call :LogSuccess "%SUCCESS_VDD%"
        ) else (
            call :LogWarning "%WARNING_VDD%"
        )
    ) else (
        call :LogInfo "%INFO_VDD_SKIP%"
    )
) else (
    call :LogInfo "%INFO_VDD_EXISTS%"
)
echo.

REM Step 2 - Install virtual microphone (VB-Cable)
call :LogStep "2/4" "%STEP_VSINK%..."
REM Check if virtual microphone is already installed
reg query "HKLM\SOFTWARE\VB-Audio" >nul 2>&1
if !errorLevel! neq 0 (
    set "vsink_choice=Y"
    set /p vsink_choice="%VSINK_PROMPT% "
    if /i "!vsink_choice!"=="Y" (
        call :LogInfo "%INFO_VSINK_INSTALL%..."
        REM Call virtual microphone installation script
        call "%SCRIPT_DIR%\install-vsink.bat"
        if !errorLevel! equ 0 (
            call :LogSuccess "%SUCCESS_VSINK%"
        ) else (
            call :LogWarning "%WARNING_VSINK%"
        )
    ) else (
        call :LogInfo "%INFO_VSINK_SKIP%"
    )
) else (
    call :LogInfo "%INFO_VSINK_EXISTS%"
)
echo.

REM Step 3 - Configure firewall rules
call :LogStep "3/4" "%STEP_FIREWALL%..."
set "firewall_choice=Y"
set /p firewall_choice="%FIREWALL_PROMPT% "
if /i "!firewall_choice!"=="Y" (
    call :LogInfo "%INFO_FIREWALL_INSTALL%"
    REM Call firewall configuration script
    call "%SCRIPT_DIR%\add-firewall-rule.bat"
) else (
    call :LogInfo "%INFO_FIREWALL_SKIP%"
)
echo.

REM Step 4 - Install gamepad support
call :LogStep "4/5" "%STEP_GAMEPAD%..."
set "gamepad_choice=Y"
set /p gamepad_choice="%GAMEPAD_PROMPT% "
if /i "!gamepad_choice!"=="Y" (
    call "%SCRIPT_DIR%\install-gamepad.bat"
) else (
    call :LogInfo "%INFO_GAMEPAD_SKIP%"
)
echo.

REM ================================================
REM Create startup script (always create)
REM ================================================

call :LogInfo "%INFO_STARTING%"
REM Create Sunshine startup script
echo Set WshShell = CreateObject^(^"WScript.Shell^"^) > "!SUNSHINE_ROOT!start_sunshine.vbs"
echo WshShell.Run ^"!SUNSHINE_ROOT!sunshine.exe^", 0, False >> "!SUNSHINE_ROOT!start_sunshine.vbs"
call :LogSuccess "%SUCCESS_STARTUP_SCRIPT%"
echo.

REM ================================================
REM Autostart configuration (optional)
REM ================================================

REM Ask user if they want to configure autostart
set "autostart_choice=Y"
set /p autostart_choice="%AUTOSTART_PROMPT% "
if /i "!autostart_choice!"=="Y" (
    call :LogInfo "%INFO_AUTOSTART_INSTALL%..."
    REM Create Sunshine shortcut
    set "SHORTCUT_PATH=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Sunshine_portable.lnk"
    echo !SHORTCUT_PATH!
    REM Use PowerShell to create shortcut
    powershell -Command "try { $WshShell = New-Object -comObject WScript.Shell; $Shortcut = $WshShell.CreateShortcut('!SHORTCUT_PATH!'); $Shortcut.TargetPath = '!SUNSHINE_ROOT!start_sunshine.vbs'; $Shortcut.WorkingDirectory = '!SUNSHINE_ROOT!'; $Shortcut.Description = 'Sunshine Portable'; $Shortcut.Save(); exit 0 } catch { exit 1 }"
    
    if !errorLevel! equ 0 (
        call :LogSuccess "%SUCCESS_AUTOSTART%"
    ) else (
        call :LogWarning "%WARNING_AUTOSTART%"
    )
) else (
    call :LogInfo "%INFO_AUTOSTART_SKIP%"
)
echo.

:SkipInstallation
REM ================================================
REM Completion message
REM ================================================

echo ================================================
echo           %TITLE_SUCCESS%
echo ================================================
echo.
call :LogSuccess "%SUCCESS_MAIN%"
echo.
echo %USAGE_1%
echo %NOTE_FIRST%
echo.

echo %SCRIPT_COMPLETE_PROMPT%
pause
exit /b 0

REM ================================================
REM Function definition area
REM ================================================

:LogInfo
REM Function - Output info level log (white)
echo [信息] %~1
goto :eof

:LogSuccess
REM Function - Output success level log (green)
powershell -c "Write-Host '[成功] %~1' -ForegroundColor Green"
goto :eof

:LogWarning
REM Function - Output warning level log (yellow)
powershell -c "Write-Host '[警告] %~1' -ForegroundColor Yellow"
goto :eof

:LogError
REM Function - Output error level log (red)
powershell -c "Write-Host '[错误] %~1' -ForegroundColor Red"
goto :eof

:LogStep
REM Function - Output step level log (cyan)
powershell -c "Write-Host '[%~1] %~2' -ForegroundColor Cyan"
goto :eof

:DetectLanguage
REM Function - Detect system language and select corresponding language file
REM Parameters - None
REM Returns - Set SELECTED_LANG variable

REM Get system language code
REM Use PowerShell to get system language for better compatibility
for /f "tokens=*" %%i in ('powershell -c "[System.Globalization.CultureInfo]::CurrentCulture.Name" 2^>nul') do set "LOCALE=%%i"

REM If PowerShell fails, use default language
if "%LOCALE%"=="" set "LOCALE=en-US"

REM Select corresponding language file based on system language code
if "%LOCALE:~0,2%"=="zh" (
    set "SELECTED_LANG=zh-Simple"
    set "SELECTED_LANG_DISPLAY=简体中文"
) else if "%LOCALE:~0,2%"=="de" (
    set "SELECTED_LANG=de-DE"
    set "SELECTED_LANG_DISPLAY=Deutsch"
) else if "%LOCALE:~0,2%"=="fr" (
    set "SELECTED_LANG=fr-FR"
    set "SELECTED_LANG_DISPLAY=Français"
) else if "%LOCALE:~0,2%"=="ja" (
    set "SELECTED_LANG=ja-JP"
    set "SELECTED_LANG_DISPLAY=日本語"
) else (
    REM Default to English
    set "SELECTED_LANG=en-US"
    set "SELECTED_LANG_DISPLAY=English"
)
goto :eof

:LoadLanguageFile
REM Function - Load specified language file
REM Parameters - %%1 = Language code (e.g. zh-CN, en-US)
REM Returns - Set all language variables

REM Build language file path
set "LANG_FILE=%LANG_DIR%\%~1.lang"

REM Check if language file exists
if not exist "%LANG_FILE%" (
    echo [ERROR] Language file not found: %LANG_FILE%
    echo [ERROR] Language file does not exist: %LANG_FILE%
    pause
    exit /b 1
)

REM Parse language file and set variables
for /f "usebackq tokens=1* delims==" %%a in ("%LANG_FILE%") do (
    if not "%%a"=="" if not "%%a:~0,1%"=="#" (
        if not "%%b"=="" (
            set "%%a=%%b" 2>nul
        )
    )
)

REM Check if this is the first time loading language (show confirmation)
if not defined LANG_CONFIRMED (
    call :LogInfo "%LANG_DETECTED%: !SELECTED_LANG_DISPLAY!"
    call :LogInfo "%LANG_CONFIRM%"

    set "lang_confirm_input=Y"
    set /p lang_confirm_input="%LANG_CONFIRM_PROMPT% "

    if /i "!lang_confirm_input!"=="N" (
        call :ShowLanguageSelection
    ) else (
        set "LANG_CONFIRMED=1"
    )
)
goto :eof

:ShowLanguageSelection
REM Function - Display language selection menu
REM Parameters - None
REM Returns - Reload selected language file

echo.
echo %LANG_SELECT%:
echo 1. %LANG_ZH_SIMPLE% (简体中文)
echo 2. %LANG_EN% (English)
echo 3. %LANG_DE% (Deutsch)
echo 4. %LANG_FR% (Français)
echo 5. %LANG_JA% (日本語)
echo 6. %LANG_RU% (Русский)
echo.
set "lang_choice="
set /p lang_choice="%LANG_SELECT_PROMPT% "

REM Set language code based on user selection
if "!lang_choice!"=="1" (
    set "SELECTED_LANG=zh-Simple"
    set "SELECTED_LANG_DISPLAY=简体中文"
) else if "!lang_choice!"=="2" (
    set "SELECTED_LANG=en-US"
    set "SELECTED_LANG_DISPLAY=English"
) else if "!lang_choice!"=="3" (
    set "SELECTED_LANG=de-DE"
    set "SELECTED_LANG_DISPLAY=Deutsch"
) else if "!lang_choice!"=="4" (
    set "SELECTED_LANG=fr-FR"
    set "SELECTED_LANG_DISPLAY=Français"
) else if "!lang_choice!"=="5" (
    set "SELECTED_LANG=ja-JP"
    set "SELECTED_LANG_DISPLAY=日本語"
) else if "!lang_choice!"=="6" (
    set "SELECTED_LANG=ru-RU"
    set "SELECTED_LANG_DISPLAY=Русский"
) else (
    call :LogError "%ERROR_INVALID_SELECTION%"
    set "SELECTED_LANG=en-US"
    set "SELECTED_LANG_DISPLAY=English"
)

REM Reload selected language file
call :LoadLanguageFile "%SELECTED_LANG%"
goto :eof

:ShowScriptConfirmation
REM Function - Display script execution confirmation interface
REM Parameters - None
REM Returns - Set CONFIRM_RESULT variable (1=confirm, 0=cancel)

echo ================================================
echo           %SCRIPT_CONFIRM_TITLE%
echo ================================================
echo.
echo %SCRIPT_CONFIRM_DESC%:
echo.
echo 1. %SCRIPT_CONFIRM_VDD%
echo 2. %SCRIPT_CONFIRM_VSINK%
echo 3. %SCRIPT_CONFIRM_FIREWALL%
echo 4. %SCRIPT_CONFIRM_GAMEPAD%
echo 5. %SCRIPT_CONFIRM_AUTOSTART%
echo.
set "confirm_choice=Y"
set /p confirm_choice="%SCRIPT_CONFIRM_PROCEED% "
if /i "!confirm_choice!"=="Y" (
    set "CONFIRM_RESULT=1"
) else (
    set "CONFIRM_RESULT=0"
)
goto :eof
