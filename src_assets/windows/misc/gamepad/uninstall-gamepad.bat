@echo off
setlocal enabledelayedexpansion

rem Uninstall ViGEm Bus Driver via registry UninstallString
rem (Replaces slow "wmic product" which can hang for minutes)

set "FOUND=0"

rem Search Uninstall registry keys for ViGEmBus
for %%R in (
  "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
  "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
) do (
  for /f "tokens=*" %%K in ('reg query %%R /s /f "ViGEm Bus Driver" /d 2^>nul ^| findstr /i "HKEY_"') do (
    for /f "tokens=2*" %%A in ('reg query "%%K" /v UninstallString 2^>nul ^| findstr /i "UninstallString"') do (
      set "UNINSTALL_CMD=%%B"
      set "FOUND=1"
    )
  )
)

if "!FOUND!"=="0" (
  rem Try finding by DisplayName containing "ViGEm"
  for %%R in (
    "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
  ) do (
    for /f "tokens=*" %%K in ('reg query %%R /s /f "ViGEm" /d 2^>nul ^| findstr /i "HKEY_"') do (
      for /f "tokens=2*" %%A in ('reg query "%%K" /v UninstallString 2^>nul ^| findstr /i "UninstallString"') do (
        set "UNINSTALL_CMD=%%B"
        set "FOUND=1"
      )
    )
  )
)

if "!FOUND!"=="0" (
  echo ViGEm Bus Driver not found in registry, nothing to uninstall.
  exit /b 0
)

echo Uninstalling ViGEm Bus Driver...
echo Command: !UNINSTALL_CMD!

rem MSI uninstall: replace /I with /X and add /qn for silent
echo !UNINSTALL_CMD! | findstr /i "msiexec" >nul
if !ERRORLEVEL!==0 (
  set "SILENT_CMD=!UNINSTALL_CMD:/I=/X!"
  !SILENT_CMD! /qn
) else (
  rem Non-MSI installer: try running with /S or /silent flag
  !UNINSTALL_CMD! /passive /norestart
)

echo ViGEm Bus Driver uninstall completed.
