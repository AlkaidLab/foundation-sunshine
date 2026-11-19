# Make an installation folder safe to update: the service stopped, nothing of the folder still
# running, no file of it held by anything else. The installer runs this before it copies a file,
# so a busy folder is found while it still holds the version it had before the update.
#
# Authority: [installer-inno-script-events-20261107] (PrepareToInstall: a returned text stops Setup
# before it changes anything), [installer-inno-exit-codes-20261119] (exit code 7),
# [installer-ms-terminateprocess-20261113] (ending a process by identity),
# [installer-ms-createfile-sharing-20261119] (an open with no sharing fails while any handle is open).
# What Setup does on a rollback is taken from its own log of the failed update: it removes the files
# it created in that run and does not bring back a file it overwrote.
#
# Exit codes: 0 ready, 10 a process of the folder is still running, 20 a file of the folder is held
# by something else (or could not be moved aside), 30 the service did not stop. The result text goes to -Report (UTF-8) so the
# installer can show it.
param(
  [Parameter(Mandatory = $true)]
  [string] $Root,
  [string] $Report = '',
  [int] $ServiceTimeoutSeconds = 60,
  [int] $ProcessTimeoutSeconds = 30,
  [int] $LockTimeoutSeconds = 15,
  [int] $QuietMilliseconds = 3500,
  # Report only: stop nothing. The installer calls it again right before the copy starts.
  [switch] $VerifyOnly,
  # Move the programs that something could start again (the host, the service, the tray) out of the
  # way once they have been ended, so that nothing can start them while the files are copied.
  [switch] $Park
)

$ErrorActionPreference = 'Continue'
$rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
$lines = New-Object System.Collections.Generic.List[string]

function Add-Line([string] $Text) { $lines.Add($Text) }

function Save-Report {
  if ([string]::IsNullOrWhiteSpace($Report)) { return }
  try { [IO.File]::WriteAllText($Report, ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($false))) } catch { }
}

function Test-InRoot([string] $Path) {
  if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
  try { return [IO.Path]::GetFullPath($Path).StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase) } catch { return $false }
}

function Get-OwnAncestors {
  # This script and the installer processes above it are never ended, wherever the folder is.
  $ids = New-Object System.Collections.Generic.HashSet[int]
  $current = $PID
  for ($i = 0; $i -lt 8 -and $current -gt 0; $i++) {
    [void] $ids.Add([int] $current)
    $parent = (Get-CimInstance Win32_Process -Filter "ProcessId=$current" -ErrorAction SilentlyContinue).ParentProcessId
    if (-not $parent -or $ids.Contains([int] $parent)) { break }
    $current = [int] $parent
  }
  return ,$ids
}

function Get-FolderProcesses {
  $own = Get-OwnAncestors
  $staging = $rootFull + 'updates\'
  $guiImage = $rootFull + 'assets\gui\sunshine-gui.exe'
  Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    if (-not $_.ExecutablePath -or -not (Test-InRoot $_.ExecutablePath)) { return $false }
    # The installer's own copies wait in updates\; what started this script is left alone, except
    # the installed Control Panel when it is the updater that started the installer: it holds
    # sunshine-gui.exe, the file that has to be replaced.
    if ([IO.Path]::GetFullPath($_.ExecutablePath).StartsWith($staging, [StringComparison]::OrdinalIgnoreCase)) { return $false }
    if ($own.Contains([int] $_.ProcessId)) {
      return ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $guiImage)
    }
    return $true
  }
}

function Get-FolderService {
  Get-CimInstance Win32_Service -ErrorAction SilentlyContinue | Where-Object {
    $path = $_.PathName
    if ($path -match '^\s*"([^"]+)"') { $path = $Matches[1] } elseif ($path) { $path = ($path -split '\s+', 2)[0] }
    Test-InRoot $path
  }
}

# Files that belong to the person, not to the program (the installer never replaces them), and the
# installer's own copies in updates\, which are running programs by design.
$userData = @('config', 'logs', 'log', 'cache', 'crash', 'crashes', 'dumps', 'updates')

function Get-HeldFiles {
  $held = New-Object System.Collections.Generic.List[string]
  $top = $rootFull.TrimEnd('\')
  Get-ChildItem -LiteralPath $top -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
    $relative = $_.FullName.Substring($rootFull.Length)
    $first = ($relative -split '\\', 2)[0]
    if ($relative.Contains('\') -and ($userData -contains $first.ToLowerInvariant())) { return }
    # The uninstaller and its data belong to Setup itself; a parked program is waiting to be deleted.
    if ($relative -match '^unins\d+\.(exe|dat|msg)$') { return }
    if ($relative.EndsWith('.parked', [StringComparison]::OrdinalIgnoreCase)) { return }
    try {
      $stream = [IO.File]::Open($_.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
      $stream.Dispose()
    } catch [System.UnauthorizedAccessException] {
      # An ACL or read-only file is not a lock; Setup resets those itself.
    } catch [System.IO.IOException] {
      $held.Add($relative)
    } catch { }
  }
  return $held
}

# The programs that are started by something else (the service starts the host and the tray, the
# host starts the tray). Renamed, they cannot be started, whatever tries; Setup puts the new ones in
# their place and deletes these, or, when it does not finish, puts these back.
$parkTargets = @('sunshine.exe', 'tools\sunshinesvc.exe', 'assets\gui\sunshine-gui.exe')

function Move-ImagesAside {
  $failed = New-Object System.Collections.Generic.List[string]
  foreach ($relative in $parkTargets) {
    $path = Join-Path $rootFull.TrimEnd('\') $relative
    $parked = $path + '.parked'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    if (Test-Path -LiteralPath $parked) { Remove-Item -LiteralPath $parked -Force -ErrorAction SilentlyContinue }
    try {
      Move-Item -LiteralPath $path -Destination $parked -Force -ErrorAction Stop
      Add-Line "parked: $relative"
    } catch {
      $failed.Add($relative)
    }
  }
  return $failed
}

if (-not (Test-Path -LiteralPath $rootFull.TrimEnd('\') -PathType Container)) {
  Add-Line 'ready: the folder does not exist yet'
  Save-Report
  exit 0
}

if (-not $VerifyOnly) {
  # 1. The service first: it supervises the tray and the host, so ending them under it only makes it
  #    start them again.
  $service = Get-FolderService | Select-Object -First 1
  if ($service -and $service.State -ne 'Stopped') {
    Add-Line "service: stopping $($service.Name)"
    & sc.exe stop $service.Name 2>&1 | Out-Null
    $deadline = (Get-Date).AddSeconds($ServiceTimeoutSeconds)
    do {
      Start-Sleep -Milliseconds 300
      $state = (Get-CimInstance Win32_Service -Filter "Name='$($service.Name)'" -ErrorAction SilentlyContinue).State
    } while ($state -and $state -ne 'Stopped' -and (Get-Date) -lt $deadline)
    if ($state -and $state -ne 'Stopped') {
      Add-Line "service: $($service.Name) is still $state after $ServiceTimeoutSeconds s"
      Save-Report
      exit 30
    }
  }

  # 2. What is left of the folder: end it by identity until nothing comes back for a quiet period,
  #    since something may start a tray again a moment after it was ended.
  $deadline = (Get-Date).AddSeconds($ProcessTimeoutSeconds)
  $quietSince = $null
  while ((Get-Date) -lt $deadline) {
    $running = @(Get-FolderProcesses)
    if ($running.Count -eq 0) {
      if (-not $quietSince) { $quietSince = Get-Date }
      if (((Get-Date) - $quietSince).TotalMilliseconds -ge $QuietMilliseconds) { break }
      Start-Sleep -Milliseconds 200
      continue
    }
    $quietSince = $null
    # supervisors before what they supervise
    $order = @{ 'sunshinesvc.exe' = 0; 'sunshine.exe' = 1 }
    foreach ($process in ($running | Sort-Object { if ($order.ContainsKey($_.Name.ToLowerInvariant())) { $order[$_.Name.ToLowerInvariant()] } else { 2 } })) {
      Add-Line "ending: $($process.Name) (PID $($process.ProcessId))"
      # The Control Panel's updater stops the service before it starts the installer and, once it is
      # ended here, nobody starts the service again after a refusal: say so, so the installer does.
      if ($process.CommandLine -and $process.CommandLine.Contains('--updater-helper')) { Add-Line 'updater: ended (it had stopped the service)' }
      Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 300
  }
}

if ($Park) {
  $notMoved = @(Move-ImagesAside)
  if ($notMoved.Count -gt 0) {
    foreach ($file in $notMoved) { Add-Line "held: $file" }
    Save-Report
    exit 20
  }
  if (-not $VerifyOnly) {
    # A program that was still running from an image (renaming a running program works) is ended now;
    # nothing can start another one.
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
      $running = @(Get-FolderProcesses)
      if ($running.Count -eq 0) { break }
      foreach ($process in $running) {
        Add-Line "ending: $($process.Name) (PID $($process.ProcessId))"
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
      }
      Start-Sleep -Milliseconds 300
    }
  }
}

$remaining = @(Get-FolderProcesses)
if ($remaining.Count -gt 0) {
  foreach ($process in $remaining) { Add-Line "running: $($process.Name) (PID $($process.ProcessId))" }
  Save-Report
  exit 10
}

# 3. Nothing of the folder runs any more. A file still held belongs to another program (a virus scan
#    in the middle of a file, an editor, a viewer). Wait a little for it, then say which one.
$deadline = (Get-Date).AddSeconds($LockTimeoutSeconds)
do {
  $held = @(Get-HeldFiles)
  if ($held.Count -eq 0) { break }
  Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $deadline)
if ($held.Count -gt 0) {
  foreach ($file in ($held | Select-Object -First 20)) { Add-Line "held: $file" }
  Save-Report
  exit 20
}

Add-Line 'ready'
Save-Report
exit 0
