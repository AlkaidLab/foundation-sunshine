#requires -version 5.1
[CmdletBinding()]
param(
  [ValidateSet('install', 'uninstall', 'remove', 'status', 'verify', 'replace')][string]$Action = 'install',
  [string]$Payload,
  [string]$InstallRoot,
  [string]$State,
  [string]$HostRoot,
  # Regression seam: exercised only on an explicitly authorized test machine.
  [int]$SimulateHubProblem = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'usbip-package-functions.psm1') -Force -DisableNameChecking
if (-not $Payload) { $Payload = Join-Path $PSScriptRoot 'usbip' }
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:ProgramFiles 'USBip' }
if (-not $HostRoot) { $HostRoot = Split-Path -Parent $PSScriptRoot }
Import-Module (Join-Path $PSScriptRoot 'usbip-package-ownership.psm1') -Force -DisableNameChecking
# `replace` is the one action that may touch an install we did not prove to be ours: the person asked for it in the
# panel and confirmed what it does. It then runs as an install that takes over the registered upstream package.
# Refused (107, nothing changed) unless exactly one upstream registration with its own uninstaller is there.
$confirmed = $Action -eq 'replace'
if ($confirmed) { $Action = 'install' }
# Decide before creating state/log files, taking the lifecycle mutex or invoking
# any guard. External ownership overrides every install and removal action.
$ownership = Get-UsbipOwnership $InstallRoot $HostRoot -ConfirmedReplace:$confirmed
if ($confirmed -and $ownership.ownership -ne 'legacy-owned') {
  Write-Output 'USB/IP cannot be replaced from here; no changes were made.'
  exit 107
}
if ($Action -ne 'verify' -and $ownership.ownership -eq 'external') {
  if ($Action -eq 'status') {
    $ready = $ownership.compatible -and (Test-UsbipExternalReady $InstallRoot)
    [pscustomobject]@{ ownership = 'external'; compatible = $ownership.compatible; reason = $ownership.reason; ready = $ready; replaceable = $ownership.replaceable; version = $ownership.version } | ConvertTo-Json -Compress
    exit 0
  }
  Write-Output 'USB/IP is externally managed; no changes were made.'
  if ($Action -eq 'install' -and -not $ownership.compatible) { exit 106 }
  exit 0
}
if ($Action -in 'uninstall', 'remove' -and $ownership.ownership -eq 'absent') { exit 0 }
if (-not $State) {
  $stateDir = Join-Path $env:ProgramData 'AlkaidLab\USBip'
  if ($Action -in 'install', 'uninstall', 'remove') { New-Item -ItemType Directory -Path $stateDir -Force | Out-Null }
  $State = Join-Path $stateDir 'roothub-state.json'
}
$guard = Join-Path $PSScriptRoot 'usbip-roothub-guard.ps1'
$powershell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$stepsRoot = if ($PSScriptRoot -eq $InstallRoot) { Split-Path -Parent $State } else { $PSScriptRoot }
$steps = Join-Path $stepsRoot 'result-usbip-steps.txt'
function Write-Step([string]$Name, [int]$Code) { Add-Content -LiteralPath $steps -Value "$Name=$Code" -Encoding ASCII }
function Invoke-Guard([string]$Operation) {
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $guard, '-Action', $Operation, '-State', $State)
  if ($Operation -eq 'verify') {
    $arguments += @('-RollbackScript', (Join-Path $InstallRoot 'usbip-package.ps1'))
    if ($SimulateHubProblem) { $arguments += @('-SimulateHubProblem', [string]$SimulateHubProblem) }
  }
  & $powershell @arguments | Out-Host
  Write-Step $Operation $LASTEXITCODE
  return $LASTEXITCODE
}

$mutex = $null
$locked = $false
$rc = 1
try {
  if ($Action -eq 'verify') { $null = Read-UsbipManifest $Payload; exit 0 }
  if ($Action -eq 'status') {
    $ready = $false
    try { $manifest = Read-UsbipManifest $InstallRoot; $ready = Test-UsbipReady $InstallRoot $manifest } catch { }
    [pscustomobject]@{ ownership = $ownership.ownership; compatible = $ownership.compatible; layout = 'usbip-slim-v1'; ready = $ready; legacy = @(Get-UsbipLegacyEntries).Count -gt 0; replaceable = $false; version = $ownership.version } | ConvertTo-Json -Compress
    exit 0
  }
  # Internal rollback is called by a child guard while this process owns the
  # lifecycle mutex. It must not acquire that mutex recursively.
  if ($Action -eq 'remove') { Remove-UsbipPackage $InstallRoot; exit (Get-UsbipRestartCode) }
  $mutex = New-Object Threading.Mutex($false, 'Global\AlkaidLabUsbipLifecycle')
  try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
  if (-not $locked) { throw 'Another USB/IP install or uninstall is running' }
  # Ownership read before the lock can be stale: another lifecycle process may have changed the installation while this
  # one waited. Decide again under the lock and apply the same external/absent rules before anything is touched.
  $ownership = Get-UsbipOwnership $InstallRoot $HostRoot -ConfirmedReplace:$confirmed
  if ($confirmed -and $ownership.ownership -ne 'legacy-owned') { Write-Output 'USB/IP cannot be replaced from here; no changes were made.'; $rc = 107; exit 107 }
  if ($ownership.ownership -eq 'external') {
    Write-Output 'USB/IP is externally managed; no changes were made.'
    if ($Action -eq 'install' -and -not $ownership.compatible) { $rc = 106; exit 106 }
    $rc = 0; exit 0
  }
  if ($Action -eq 'uninstall' -and $ownership.ownership -eq 'absent') { $rc = 0; exit 0 }
  if (Test-Path -LiteralPath $steps) { Remove-Item -LiteralPath $steps -Force }
  if ($Action -eq 'install') {
    $manifest = Read-UsbipManifest $Payload
    Write-Step payload 0
    if ((Test-UsbipReady $InstallRoot $manifest) -and @(Get-UsbipLegacyEntries).Count -eq 0) { exit 0 }
    $rc = Invoke-Guard preflight
    if ($rc -ne 0) { exit $rc }
  }
  if ((Invoke-Guard snapshot) -ne 0) { exit 105 }
  $operationFailed = $false
  try {
    if ($Action -eq 'install') { Install-UsbipPackage $Payload $InstallRoot $PSScriptRoot $HostRoot -ConfirmedReplace:$confirmed }
    else {
      Remove-UsbipLegacy $InstallRoot $HostRoot
      if ($ownership.ownership -eq 'managed') { Remove-UsbipPackage $InstallRoot -KeepFiles }
    }
    Write-Step $Action 0
  } catch {
    Write-Warning $_.Exception.Message
    Write-Step $Action 1
    $operationFailed = $true
    if ($Action -eq 'install') {
      try { Remove-UsbipPackage $InstallRoot; Write-Step cleanup 0 }
      catch { Write-Warning $_.Exception.Message; Write-Step cleanup 1 }
    }
  }
  if ($Action -eq 'uninstall' -or $operationFailed) {
    if ((Invoke-Guard cleanup) -ne 0) { $operationFailed = $true }
  }
  # Always verify, even when a driver command failed after changing state.
  $rc = Invoke-Guard verify
  if ($rc -in 0, 3010, 102) { Remove-Item -LiteralPath $State -Force -ErrorAction SilentlyContinue }
  if (($rc -in 0, 3010) -and -not $operationFailed -and $Action -eq 'uninstall') { Remove-UsbipFiles $InstallRoot }
  if ($rc -eq 0) { $rc = if ($operationFailed) { 1 } else { Get-UsbipRestartCode } }
  elseif ($rc -eq 3010 -and $operationFailed) { $rc = 1 }
} catch {
  Write-Warning $_.Exception.Message
  $rc = 1
} finally {
  if ($locked) { $mutex.ReleaseMutex() }
  if ($null -ne $mutex) { $mutex.Dispose() }
}
exit $rc
