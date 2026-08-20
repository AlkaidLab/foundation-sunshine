@echo off
rem ============================================================================
rem update-path.bat <add|remove>
rem
rem Adds or removes the Sunshine install directory and its `tools\` subdir to
rem the *machine* PATH (HKLM Session Manager Environment).
rem
rem Implementation note: previous pure-batch versions parsed PATH with delayed
rem expansion + `for` substring tricks. That is O(n^2) on PATH length and
rem brittle on PATHs containing `!`, `&` or unbalanced quotes — large machines
rem hit this and the Inno uninstaller appeared to hang. This version offloads
rem the parsing to PowerShell and writes the registry directly via
rem Set-ItemProperty (REG_EXPAND_SZ, no WM_SETTINGCHANGE broadcast — so no
rem 5-second SendMessageTimeout wait per top-level window).
rem ============================================================================
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"

if "%~1"=="" (
    echo Usage: %~nx0 [add^|remove]
    exit /b 1
)

set "SUNSHINE_PATH_ACTION=%~1"
for %%I in ("%~dp0\..") do set "SUNSHINE_PATH_ROOT=%%~fI"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$action = $env:SUNSHINE_PATH_ACTION;" ^
  "$root   = $env:SUNSHINE_PATH_ROOT;" ^
  "$tools  = Join-Path $root 'tools';" ^
  "$key    = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment';" ^
  "$cur    = (Get-ItemProperty -Path $key -Name Path -ErrorAction Stop).Path;" ^
  "$parts  = @($cur -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' });" ^
  "$targets = @($root, $tools);" ^
  "switch ($action.ToLower()) {" ^
  "  'add'    { foreach ($t in $targets) { if (-not ($parts -icontains $t)) { $parts += $t } } }" ^
  "  'remove' { $parts = @($parts | Where-Object { -not ($targets -icontains $_) }) }" ^
  "  default  { Write-Host (\"Unknown action: \" + $action); exit 2 }" ^
  "}" ^
  "$new = ($parts -join ';');" ^
  "if ($new -ne $cur) {" ^
  "  Set-ItemProperty -Path $key -Name Path -Value $new -Type ExpandString;" ^
  "  Write-Host 'PATH updated.';" ^
  "} else {" ^
  "  Write-Host 'PATH unchanged.';" ^
  "}"

exit /b %ERRORLEVEL%
