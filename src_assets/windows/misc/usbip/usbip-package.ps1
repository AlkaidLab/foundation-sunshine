#requires -version 5.1
[CmdletBinding()]
param(
  [ValidateSet('install', 'uninstall', 'remove', 'status', 'verify')][string]$Action = 'install',
  [string]$Payload,
  [string]$InstallRoot,
  [string]$State,
  # Regression seam: exercised only on an explicitly authorized test machine.
  [int]$SimulateHubProblem = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'usbip-package-functions.psm1') -Force -DisableNameChecking
if (-not $Payload) { $Payload = Join-Path $PSScriptRoot 'usbip' }
if (-not $InstallRoot) { $InstallRoot = Join-Path $env:ProgramFiles 'USBip' }
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
    [pscustomobject]@{ layout = 'usbip-slim-v1'; ready = $ready; legacy = @(Get-UsbipLegacyEntries).Count -gt 0 } | ConvertTo-Json -Compress
    exit 0
  }
  # Internal rollback is called by a child guard while this process owns the
  # lifecycle mutex. It must not acquire that mutex recursively.
  if ($Action -eq 'remove') { Remove-UsbipPackage $InstallRoot; exit (Get-UsbipRestartCode) }
  $mutex = New-Object Threading.Mutex($false, 'Global\AlkaidLabUsbipLifecycle')
  try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
  if (-not $locked) { throw 'Another USB/IP install or uninstall is running' }
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
    if ($Action -eq 'install') { Install-UsbipPackage $Payload $InstallRoot $PSScriptRoot }
    else {
      Remove-UsbipLegacy
      Remove-UsbipPackage $InstallRoot -KeepFiles
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
