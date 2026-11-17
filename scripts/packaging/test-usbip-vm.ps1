#requires -version 5.1
# Explicit controller-run acceptance on an allocated Windows test machine.
param(
  [Parameter(Mandatory)][switch]$AllowMachineChanges,
  [ValidateSet('All', 'Fresh', 'Upgrade', 'Uninstall', 'Rollback', 'Devices')][string]$Scenario = 'All',
  [string]$RemoteUsbProbe,
  [string]$NativeProbe
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $AllowMachineChanges) { throw 'An allocated machine and explicit test authorization are required' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$work = Split-Path -Parent $root
$results = Join-Path $work 'acceptance-results.json'
$records = [Collections.Generic.List[object]]::new()
$pending = $false
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$scripts = Join-Path $root 'src_assets\windows\misc\usbip'
$package = Join-Path $scripts 'usbip-package.ps1'
$payload = Join-Path $work 'payload'
$installed = Join-Path $env:ProgramFiles 'USBip'
$upstream = Join-Path $work 'USBip-0.9.8.1-x64.exe'
function Record([string]$Name, [string]$Result) {
  $records.Add([pscustomobject]@{ name = $Name; result = $Result; utc = [DateTime]::UtcNow.ToString('o') })
  $records.ToArray() | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $results -Encoding UTF8
  Write-Host "$Name : $Result"
}
function Require($Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Run-Package([string]$Action, [switch]$Fault) {
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $package, '-Action', $Action, '-Payload', $payload)
  if ($Fault) { $arguments += @('-SimulateHubProblem', '19') }
  & $ps @arguments | Out-Host
  $code = $LASTEXITCODE
  if ($Fault) { Require ($code -eq 102) "rollback returned $code instead of 102" }
  else { Require ($code -in 0, 3010) "$Action returned $code" }
  Record "$Action$(if ($Fault) { '-fault' })" "exit $code"
  if ($code -eq 3010) { $script:pending = $true; Record 'restart' 'required; controller must arrange it, this script never restarts' }
}
Import-Module (Join-Path $scripts 'usbip-package-functions.psm1') -Force -DisableNameChecking
function Assert-Ready {
  Require (Test-UsbipReady $installed (Read-UsbipManifest $installed)) 'installed transport is not ready'
  Require (@(Get-UsbipLegacyEntries).Count -eq 0) 'legacy registration remains alongside the new receipt'
  Require (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AlkaidLabUSBip') 'slim receipt missing'
}
function Assert-Removed {
  Require (@(Get-UsbipOemPackages).Count -eq 0) 'driver packages remain'
  Require (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AlkaidLabUSBip')) 'slim receipt remains'
  Require (-not (Test-Path -LiteralPath (Join-Path $installed 'payload.json'))) 'payload files remain'
  $scheduler = New-Object -ComObject Schedule.Service
  $scheduler.Connect()
  $taskFound = @($scheduler.GetFolder('\').GetTasks(0) | Where-Object { $_.Name -eq 'USBip Detach All On Reboot Or Shutdown' }).Count -gt 0
  Require (-not $taskFound) 'detach task remains'
}
try {
  Require (-not (Get-Process -Name sunshine, wusbip -ErrorAction SilentlyContinue)) 'a host or USB/IP session may be active; allocate an idle machine first'
  if (-not (Test-Path -LiteralPath $upstream)) {
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe' -OutFile $upstream
  }
  & python (Join-Path $PSScriptRoot 'extract-usbip.py') --installer $upstream --output $payload --scratch (Join-Path $work 'extraction')
  Require ($LASTEXITCODE -eq 0) 'extraction/signature checks failed'
  Record 'signatures' '14 fixed hashes; catalog/member and runtime signatures passed'
  if ($Scenario -in 'All', 'Fresh') {
    Require (@(Get-UsbipLegacyEntries).Count -eq 0) 'Fresh needs no upstream installation; select Upgrade for an existing upstream package'
    if (Test-Path -LiteralPath (Join-Path $installed 'payload.json')) { Run-Package uninstall }
    Assert-Removed
    Run-Package install
    Assert-Ready
    Record 'fresh' 'passed'
  }
  if ($Scenario -in 'All', 'Upgrade') {
    if (Test-Path -LiteralPath (Join-Path $installed 'payload.json')) { Run-Package uninstall }
    if (@(Get-UsbipLegacyEntries).Count -eq 0) {
      $fixtureState = Join-Path $work 'legacy-fixture-hubs.json'
      $fixtureGuard = Join-Path $scripts 'usbip-roothub-guard.ps1'
      & $ps -NoProfile -ExecutionPolicy Bypass -File $fixtureGuard -Action preflight
      Require ($LASTEXITCODE -eq 0) 'legacy fixture preflight refused'
      & $ps -NoProfile -ExecutionPolicy Bypass -File $fixtureGuard -Action snapshot -State $fixtureState
      Require ($LASTEXITCODE -eq 0) 'legacy fixture snapshot failed'
      $process = Start-Process -FilePath $upstream -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/RESTARTEXITCODE=3010', '/COMPONENTS=main,client' -Wait -PassThru
      & $ps -NoProfile -ExecutionPolicy Bypass -File $fixtureGuard -Action verify -State $fixtureState
      Require ($LASTEXITCODE -in 0, 3010) 'legacy fixture root-hub check failed or rolled back'
      Require ($process.ExitCode -in 0, 3010) "upstream fixture install failed: $($process.ExitCode)"
      if ($process.ExitCode -eq 3010) {
        Record 'upgrade-fixture' 'installed; restart required before rerunning Upgrade'
        exit 77
      }
    }
    Run-Package install
    Assert-Ready
    Record 'upgrade' 'passed; no parallel legacy registration'
  }
  if ($Scenario -in 'All', 'Uninstall') {
    Run-Package uninstall
    Assert-Removed
    Record 'uninstall' 'passed'
  }
  if ($Scenario -in 'All', 'Rollback') {
    if (Test-Path -LiteralPath (Join-Path $installed 'payload.json')) { Run-Package uninstall }
    $hubs = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\ROOT_HUB*' } | Where-Object {
      (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Parent -ErrorAction Stop).Data -notlike 'ROOT\*'
    })
    if ($hubs.Count -eq 0) { Record 'rollback' 'not verified: no root hub for the forced-problem case'; exit 77 }
    Run-Package install -Fault
    Assert-Removed
    Record 'rollback' 'passed: forced problem 19, exit 102, no driver/receipt/task remains'
  }
  if ($Scenario -in 'All', 'Devices') {
    Run-Package install
    Assert-Ready
    foreach ($probe in @(@{Name = 'remote-usb'; Path = $RemoteUsbProbe}, @{Name = 'native-controller'; Path = $NativeProbe})) {
      if (-not $probe.Path) { Record $probe.Name 'not verified: controller must supply the allocated device probe'; $pending = $true; continue }
      Require (Test-Path -LiteralPath $probe.Path -PathType Leaf) 'device probe does not exist'
      & $probe.Path 30000 | Out-Host
      Require ($LASTEXITCODE -eq 0) "$($probe.Name) failed or skipped: $LASTEXITCODE"
      Record $probe.Name 'probe passed'
    }
  }
  Record 'resource-authorization' 'not verified by lifecycle tests'
  Record 'cross-tenant-isolation' 'not verified by lifecycle tests'
  if ($pending) { exit 77 }
} catch { Record 'failure' $_.Exception.Message; exit 1 }
exit 0
