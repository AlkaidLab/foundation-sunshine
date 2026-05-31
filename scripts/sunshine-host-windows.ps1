<#
.SYNOPSIS
  Generic Sunshine host management helper for Windows.

.DESCRIPTION
  Runs locally on a Windows Sunshine host, or through SSH from the companion
  macOS script. It can report status, start Sunshine in the interactive user
  session, stop it, deploy a Sunshine.exe artifact, copy runtime assets, show
  logs, and show recent crash events.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\sunshine-host-windows.ps1 -Action status

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\sunshine-host-windows.ps1 -Action deploy-user -PackageExe C:\Temp\Sunshine.exe
#>
[CmdletBinding()]
param(
  [ValidateSet('interactive','status','logs','events','start-user','stop','deploy-user','deploy-assets','help')]
  [string]$Action = 'interactive',

  [string]$InstallDir = 'C:\Program Files\Sunshine',
  [string]$PackageExe,
  [string]$AssetsPath,
  [string]$StageDir = 'C:\ProgramData\SunshineDeployTools\stage',
  [string]$TaskName = 'SunshineUserMode',
  [string]$StartArgs = '',
  [string]$CaptureBackend = 'wgc',
  [int]$Tail = 220,
  [ValidateSet('auto','zh','en')]
  [string]$Lang = 'zh',
  [switch]$NoStartAfterDeploy,
  [switch]$AllowSystemCompanion
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-UiLang {
  if ($Lang -eq 'zh' -or $Lang -eq 'en') { return $Lang }
  try {
    $name = [System.Globalization.CultureInfo]::CurrentUICulture.Name
    if ($name -like 'zh*') { return 'zh' }
  } catch {}
  return 'en'
}

function T([string]$Key) {
  $zh = @{
    HelpTitle = '通用 Sunshine Windows Host 助手'
    Usage = '用法'
    Actions = '命令'
    Options = '选项'
    Notes = '说明'
    InteractiveTitle = 'Sunshine Host 助手'
    SelectAction = '选择操作'
    UnknownChoice = '未知选择'
    PackagePath = 'Sunshine.exe 包路径'
    AssetsPathPrompt = 'assets 文件夹或 zip 路径'
    StoppingSunshine = '正在停止 Sunshine'
    StartingInteractive = '正在 Windows 交互用户会话中启动 Sunshine'
    NoInteractive = '找不到活动的交互用户会话。请先登录 Windows 桌面。'
  }
  $en = @{
    HelpTitle = 'Generic Sunshine Windows host helper'
    Usage = 'Usage'
    Actions = 'Actions'
    Options = 'Options'
    Notes = 'Notes'
    InteractiveTitle = 'Sunshine host helper'
    SelectAction = 'Select action'
    UnknownChoice = 'Unknown choice'
    PackagePath = 'Path to Sunshine.exe package'
    AssetsPathPrompt = 'Path to assets folder or zip'
    StoppingSunshine = 'Stopping Sunshine'
    StartingInteractive = 'Starting Sunshine in interactive user session'
    NoInteractive = 'No active interactive user session found. Log into the Windows desktop first.'
  }
  if ((Get-UiLang) -eq 'zh') { return $zh[$Key] }
  return $en[$Key]
}

function Show-Help {
  if ((Get-UiLang) -eq 'zh') {
  @"
通用 Sunshine Windows Host 助手

用法:
  powershell -ExecutionPolicy Bypass -File .\sunshine-host-windows.ps1 -Action <命令> [选项]

命令:
  interactive       显示交互菜单。
  status            查看服务、进程、hash、监听端口、计划任务和启动日志。
  logs              查看 Sunshine 日志尾部。
  events            查看最近 Sunshine 的 Windows Application Error / WER 事件。
  start-user        停止服务/session-0 进程，并在交互用户会话中启动 Sunshine。
  stop              停止 SunshineService 和所有 Sunshine.exe。
  deploy-user       将 -PackageExe 复制到 InstallDir\Sunshine.exe，校验 hash，然后 start-user。
  deploy-assets     将 -AssetsPath 复制或解压到 InstallDir\assets。
  help              显示帮助。

选项:
  -Lang <auto|zh|en>        语言。默认 zh。
  -InstallDir <path>        默认 C:\Program Files\Sunshine。
  -PackageExe <path>        deploy-user 使用的本地 Windows Sunshine.exe 路径。
  -AssetsPath <path>        deploy-assets 使用的文件夹或 .zip。
  -StageDir <path>          默认 C:\ProgramData\SunshineDeployTools\stage。
  -TaskName <name>          用户态启动计划任务名。默认 SunshineUserMode。
  -StartArgs <args>         额外 Sunshine 命令行参数。
  -CaptureBackend <name>    start-user 强制写入的 capture backend。默认 wgc；传 '' 跳过。
  -Tail <n>                 日志/事件行数。默认 220。
  -NoStartAfterDeploy       deploy-user 只部署不启动。
  -AllowSystemCompanion     允许额外 SYSTEM/session-0 Sunshine 进程。

说明:
  start-user 使用 LogonType=Interactive 的计划任务启动，因此 WGC/光标采集运行在桌面用户会话，
  而不是 service session 0。
"@
    return
  }
  @"
Generic Sunshine Windows host helper

Usage:
  powershell -ExecutionPolicy Bypass -File .\sunshine-host-windows.ps1 -Action <action> [options]

Actions:
  interactive       Show an interactive menu.
  status            Show service, process, hash, listeners, task, and startup lines.
  logs              Tail Sunshine log.
  events            Show recent Windows Application Error / WER entries for Sunshine.
  start-user        Stop service/session-0 process and start Sunshine in interactive user session.
  stop              Stop SunshineService and all Sunshine.exe processes.
  deploy-user       Copy -PackageExe to InstallDir\Sunshine.exe, verify hash, then start-user.
  deploy-assets     Copy or expand -AssetsPath into InstallDir\assets.
  help              Show this help.

Options:
  -InstallDir <path>          Default: C:\Program Files\Sunshine
  -PackageExe <path>          Local Windows path to Sunshine.exe for deploy-user.
  -AssetsPath <path>          Folder or .zip/.tar archive for deploy-assets.
  -StageDir <path>            Default: C:\ProgramData\SunshineDeployTools\stage
  -TaskName <name>            Scheduled task name for user-mode launch. Default: SunshineUserMode
  -StartArgs <args>           Extra Sunshine command-line args.
  -CaptureBackend <name>      Config capture backend to enforce for start-user. Default: wgc. Use '' to skip.
  -Tail <n>                   Log/event tail count. Default: 220
  -NoStartAfterDeploy         deploy-user only: deploy but do not start.
  -AllowSystemCompanion       Allow an additional SYSTEM/session-0 Sunshine process.

Notes:
  start-user uses a scheduled task with LogonType=Interactive so WGC/cursor capture
  runs in the desktop user session, not service session 0.
"@
}

function Write-Section([string]$Title) {
  Write-Host "== $Title =="
}

function Get-SunshineExePath {
  Join-Path $InstallDir 'Sunshine.exe'
}

function Get-SunshineLogPath {
  Join-Path $InstallDir 'config\sunshine.log'
}

function Get-OwnerText([uint32]$ProcessId) {
  $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction SilentlyContinue
  if ($null -eq $proc) { return 'unknown' }
  $owner = Invoke-CimMethod -InputObject $proc -MethodName GetOwner -ErrorAction SilentlyContinue
  if ($owner -and $owner.ReturnValue -eq 0) { return "$($owner.Domain)\$($owner.User)" }
  return 'unknown'
}

function Get-SunshineProcessInfo {
  foreach ($proc in Get-CimInstance Win32_Process -Filter "Name='sunshine.exe'" -ErrorAction SilentlyContinue) {
    [PSCustomObject]@{
      ProcessId      = $proc.ProcessId
      SessionId      = $proc.SessionId
      Owner          = Get-OwnerText $proc.ProcessId
      ExecutablePath = $proc.ExecutablePath
      CommandLine    = $proc.CommandLine
    }
  }
}

function Get-InteractiveUser {
  $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
    Sort-Object CreationDate -Descending |
    Select-Object -First 1
  if ($explorer) {
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner -ErrorAction SilentlyContinue
    if ($owner -and $owner.ReturnValue -eq 0 -and $owner.User) {
      return [PSCustomObject]@{ User = "$($owner.Domain)\$($owner.User)"; SessionId = $explorer.SessionId }
    }
  }

  $consoleUser = (Get-CimInstance Win32_ComputerSystem).UserName
  if ($consoleUser) {
    return [PSCustomObject]@{ User = $consoleUser; SessionId = $null }
  }
  return $null
}

function Stop-SunshineHost {
  Write-Section (T 'StoppingSunshine')
  Stop-Service SunshineService -Force -ErrorAction SilentlyContinue
  Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1
  Show-Status
}

function Ensure-CaptureConfig {
  if ([string]::IsNullOrWhiteSpace($CaptureBackend)) { return }

  $configDir = Join-Path $InstallDir 'config'
  $configPath = Join-Path $configDir 'sunshine.conf'
  New-Item -ItemType Directory -Path $configDir -Force | Out-Null

  $configLines = @()
  if (Test-Path $configPath) {
    $configLines = @(Get-Content -Path $configPath -ErrorAction SilentlyContinue)
  }

  $updatedConfig = New-Object System.Collections.Generic.List[string]
  $captureSet = $false
  foreach ($line in $configLines) {
    if ([regex]::IsMatch($line, '^\s*capture\s*=')) {
      if (-not $captureSet) {
        $updatedConfig.Add("capture = $CaptureBackend")
        $captureSet = $true
      }
    }
    else {
      $updatedConfig.Add($line)
    }
  }
  if (-not $captureSet) { $updatedConfig.Add("capture = $CaptureBackend") }
  Set-Content -Path $configPath -Value $updatedConfig -Encoding UTF8
  Write-Host "Ensured Sunshine capture backend: capture=$CaptureBackend config=$configPath"
}

function Start-SunshineUserMode {
  $exe = Get-SunshineExePath
  if (!(Test-Path $exe)) { throw "Sunshine exe missing: $exe" }

  $interactive = Get-InteractiveUser
  if ($null -eq $interactive -or [string]::IsNullOrWhiteSpace($interactive.User)) {
    throw (T 'NoInteractive')
  }

  Write-Host "$(T 'StartingInteractive'): user=$($interactive.User) session=$($interactive.SessionId)"
  Stop-Service SunshineService -Force -ErrorAction SilentlyContinue
  Set-Service SunshineService -StartupType Manual -ErrorAction SilentlyContinue
  Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1

  Ensure-CaptureConfig

  $launcherDir = Join-Path $env:ProgramData 'SunshineDeployTools'
  New-Item -ItemType Directory -Path $launcherDir -Force | Out-Null
  $launcherPath = Join-Path $launcherDir 'start-sunshine.vbs'
  $workDir = Split-Path $exe -Parent
  $escapedExe = $exe.Replace('"', '""')
  $escapedWorkDir = $workDir.Replace('"', '""')
  $escapedArgs = $StartArgs.Replace('"', '""')
  $quotePair = [string][char]34 + [string][char]34
  if ([string]::IsNullOrWhiteSpace($escapedArgs)) {
    $runCommand = $quotePair + $escapedExe + $quotePair
  }
  else {
    $runCommand = $quotePair + $escapedExe + $quotePair + ' ' + $escapedArgs
  }

  $launcher = @"
Set shell = CreateObject("WScript.Shell")
shell.CurrentDirectory = "$escapedWorkDir"
shell.Run "$runCommand", 0, False
"@
  Set-Content -Path $launcherPath -Value $launcher -Encoding ASCII

  Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
  $wscriptExe = Join-Path $env:SystemRoot 'System32\wscript.exe'
  $action = New-ScheduledTaskAction -Execute $wscriptExe -Argument "`"$launcherPath`""
  $trigger = New-ScheduledTaskTrigger -AtLogOn -User $interactive.User
  $settings = New-ScheduledTaskSettingsSet -Compatibility Win8 -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Seconds 0)
  $principal = New-ScheduledTaskPrincipal -UserId $interactive.User -LogonType Interactive -RunLevel Highest
  Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
  Start-ScheduledTask -TaskName $TaskName
  Start-Sleep -Seconds 5

  $items = @(Get-SunshineProcessInfo)
  if ($items.Count -eq 0) { throw 'Sunshine did not start from the user-mode scheduled task.' }

  Write-Section 'Sunshine process'
  $items | Select-Object ProcessId,SessionId,Owner,ExecutablePath | Format-Table -AutoSize | Out-String -Width 4096

  $userItems = @($items | Where-Object { $_.Owner -notmatch 'SYSTEM' -and $_.SessionId -ne 0 })
  $systemItems = @($items | Where-Object { $_.Owner -match 'SYSTEM' -or $_.SessionId -eq 0 })
  if ($systemItems.Count -gt 0 -and -not $AllowSystemCompanion) {
    throw 'Sunshine is still running as SYSTEM/session 0; user-mode capture may not be reliable.'
  }
  if ($AllowSystemCompanion -and $userItems.Count -eq 0) {
    throw 'System companion is allowed, but no interactive user Sunshine process is running.'
  }

  Show-Status
}

function Deploy-SunshineExe {
  if ([string]::IsNullOrWhiteSpace($PackageExe)) { throw 'deploy-user requires -PackageExe <path-to-Sunshine.exe>' }
  if (!(Test-Path $PackageExe)) { throw "Package exe missing: $PackageExe" }

  $dst = Get-SunshineExePath
  $installRoot = Split-Path $dst -Parent
  $backupDir = Join-Path $StageDir 'backup'
  New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
  New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

  Write-Section 'Stopping Sunshine for deploy'
  Stop-Service SunshineService -Force -ErrorAction SilentlyContinue
  Set-Service SunshineService -StartupType Manual -ErrorAction SilentlyContinue
  Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1

  if (Test-Path $dst) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = Join-Path $backupDir "Sunshine.exe.$stamp.bak"
    Copy-Item -Path $dst -Destination $backup -Force
    Write-Host "Backup: $backup"
  }

  for ($i = 0; $i -lt 20; $i++) {
    try {
      Copy-Item -Path $PackageExe -Destination $dst -Force -ErrorAction Stop
      Write-Host 'Copy succeeded'
      break
    }
    catch {
      Write-Host "Copy retry ${i}: $($_.Exception.Message)"
      Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
      Start-Sleep -Milliseconds 700
      if ($i -eq 19) { throw }
    }
  }

  $installedHash = (Get-FileHash $dst -Algorithm SHA256).Hash
  $packageHash = (Get-FileHash $PackageExe -Algorithm SHA256).Hash
  Write-Section 'Installed exe hash'
  Get-FileHash $dst -Algorithm SHA256 | Format-Table -AutoSize | Out-String -Width 4096
  Write-Section 'Package exe hash'
  Get-FileHash $PackageExe -Algorithm SHA256 | Format-Table -AutoSize | Out-String -Width 4096
  if ($installedHash -ne $packageHash) {
    throw "Installed Sunshine hash mismatch: installed=$installedHash package=$packageHash"
  }

  if (-not $NoStartAfterDeploy) { Start-SunshineUserMode } else { Show-Status }
}

function Deploy-SunshineAssets {
  if ([string]::IsNullOrWhiteSpace($AssetsPath)) { throw 'deploy-assets requires -AssetsPath <folder-or-zip>' }
  if (!(Test-Path $AssetsPath)) { throw "Assets path missing: $AssetsPath" }

  $assetsDst = Join-Path $InstallDir 'assets'
  New-Item -ItemType Directory -Path $assetsDst -Force | Out-Null

  if ((Get-Item $AssetsPath).PSIsContainer) {
    Write-Host "Copying assets folder: $AssetsPath -> $assetsDst"
    robocopy $AssetsPath $assetsDst /MIR /NFL /NDL /NJH /NJS /NP | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }
  }
  elseif ($AssetsPath -match '\.zip$') {
    $tmp = Join-Path $StageDir ('assets-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    Expand-Archive -Path $AssetsPath -DestinationPath $tmp -Force
    $source = $tmp
    $children = @(Get-ChildItem $tmp)
    if ($children.Count -eq 1 -and $children[0].PSIsContainer) { $source = $children[0].FullName }
    robocopy $source $assetsDst /MIR /NFL /NDL /NJH /NJS /NP | Out-Host
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }
  }
  else {
    throw 'Unsupported assets package. Use a folder or .zip.'
  }

  Write-Section 'Assets status'
  Get-Item $assetsDst | Select-Object FullName,LastWriteTime | Format-Table -AutoSize | Out-String -Width 4096
}

function Show-Status {
  $exe = Get-SunshineExePath
  $log = Get-SunshineLogPath

  Write-Section 'Sunshine service'
  $svc = Get-Service SunshineService -ErrorAction SilentlyContinue
  if ($svc) { $svc | Select-Object Name,Status,StartType | Format-Table -AutoSize | Out-String -Width 4096 } else { Write-Host 'missing service' }

  Write-Section 'Sunshine process'
  $items = @(Get-SunshineProcessInfo)
  if ($items.Count -gt 0) {
    $items | Select-Object ProcessId,SessionId,Owner,ExecutablePath | Format-Table -AutoSize | Out-String -Width 4096
  }
  else { Write-Host 'not running' }

  Write-Section 'Installed exe hash'
  if (Test-Path $exe) { Get-FileHash $exe -Algorithm SHA256 | Select-Object Algorithm,Hash,Path | Format-Table -AutoSize | Out-String -Width 4096 } else { Write-Host "missing: $exe" }

  Write-Section 'User-mode scheduled task'
  $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
  if ($task) { $task | Select-Object TaskName,State,TaskPath | Format-Table -AutoSize | Out-String -Width 4096 } else { Write-Host "missing: $TaskName" }

  Write-Section 'GameStream listeners'
  $ports = 47984,47989,47990,47998,47999,48000,48001,48010,57984,57989,57990,57998,57999,58000,58001,58010
  Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
    Where-Object { $_.LocalPort -in $ports } |
    Select-Object LocalAddress,LocalPort,OwningProcess |
    Sort-Object LocalPort |
    Format-Table -AutoSize |
    Out-String -Width 4096

  Write-Section 'Latest startup lines'
  if (Test-Path $log) {
    Select-String -Path $log -Pattern 'Sunshine version|Running as|created encoder|WGC capture|Desktop Duplication|Backend cursor capture|RTSP|New streaming session|Error|Fatal' |
      Select-Object -Last 40 |
      ForEach-Object { $_.Line }
  }
  else { Write-Host "log missing: $log" }
}

function Show-Logs {
  $log = Get-SunshineLogPath
  Write-Section "Sunshine log tail ($Tail)"
  if (Test-Path $log) { Get-Content -Path $log -Tail $Tail } else { Write-Host "log missing: $log" }
}

function Show-Events {
  Write-Section "Recent Sunshine crash/error events ($Tail)"
  $start = (Get-Date).AddDays(-7)
  Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $start } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Application Error|Windows Error Reporting|Application Hang|Sunshine' -or $_.Message -match 'Sunshine|sunshine\.exe' } |
    Sort-Object TimeCreated -Descending |
    Select-Object -First $Tail TimeCreated,ProviderName,Id,LevelDisplayName,@{n='Message';e={($_.Message -replace "`r|`n", ' ')}} |
    Format-List |
    Out-String -Width 4096

  Write-Section 'Local CrashDumps'
  $dumpDirs = @("$env:LOCALAPPDATA\CrashDumps", 'C:\ProgramData\Microsoft\Windows\WER\Temp')
  foreach ($dir in $dumpDirs) {
    if (Test-Path $dir) {
      Get-ChildItem $dir -Filter 'Sunshine*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 20 FullName,Length,LastWriteTime |
        Format-Table -AutoSize |
        Out-String -Width 4096
    }
  }
}

function Show-InteractiveMenu {
  while ($true) {
    Write-Host ''
    Write-Host (T 'InteractiveTitle')
    Write-Host '  1) status'
    Write-Host '  2) logs'
    Write-Host '  3) events'
    Write-Host '  4) start-user'
    Write-Host '  5) stop'
    Write-Host '  6) deploy-user'
    Write-Host '  7) deploy-assets'
    Write-Host '  q) quit'
    $choice = Read-Host (T 'SelectAction')
    switch ($choice) {
      '1' { Show-Status }
      '2' { Show-Logs }
      '3' { Show-Events }
      '4' { Start-SunshineUserMode }
      '5' { Stop-SunshineHost }
      '6' {
        if ([string]::IsNullOrWhiteSpace($PackageExe)) { $script:PackageExe = Read-Host (T 'PackagePath') }
        Deploy-SunshineExe
      }
      '7' {
        if ([string]::IsNullOrWhiteSpace($AssetsPath)) { $script:AssetsPath = Read-Host (T 'AssetsPathPrompt') }
        Deploy-SunshineAssets
      }
      'q' { return }
      'Q' { return }
      default { Write-Host (T 'UnknownChoice') }
    }
  }
}

switch ($Action) {
  'help'          { Show-Help }
  'interactive'   { Show-InteractiveMenu }
  'status'        { Show-Status }
  'logs'          { Show-Logs }
  'events'        { Show-Events }
  'start-user'    { Start-SunshineUserMode }
  'stop'          { Stop-SunshineHost }
  'deploy-user'   { Deploy-SunshineExe }
  'deploy-assets' { Deploy-SunshineAssets }
  default         { Show-Help; throw "Unknown action: $Action" }
}
