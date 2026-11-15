#requires -version 5.1
<##
  Guards the usbip-win2 install against the root-hub failure of issue #1272.

  usbip-win2 ships `usbip2_filter`, an upper filter for the emulated host controller's root hub. It is a companion of
  the UDE driver (usbip2_ude): UDE does not pass SELECT_CONFIGURATION / SELECT_INTERFACE down, the filter does, so the
  native DualSense path cannot do without it. Its extension INF matches the hardware id USB\ROOT_HUB30, which every
  physical xHCI root hub has too; the driver itself skips hubs that are not above the emulated controller, but
  installing or removing the package restarts every USB 3 root hub (see drivers/ude_filter/readme.txt in
  usbip-win2) and one of those restarts can leave the physical hubs in Code 19 (CM_PROB_REGISTRY). Without a working
  hub no USB keyboard or mouse works. usbip-win2 issue #201 shows a second form: the filter name is also persisted in
  Enum\USB\ROOT_HUB30\<instance>\Properties\{a193307d-...}\0002, where removing the package alone does not clear it.

  The usbip-win2 installer cannot leave the filter out: `client` is a fixed Inno component and the pnputil step has no
  condition. So this guard checks around it:

    preflight  refuses (104) when Windows itself runs from a USB disk (usbip-win2 issue #201).
    snapshot   records the physical root hubs and the usbip driver packages before the install.
    verify     waits for the hubs to settle. A physical root hub that was fine before and now has a problem code
               (other than 14, restart needed) means the install is rolled back: the usbip2_filter package is
               deleted with `pnputil /delete-driver ... /uninstall /force`, persisted filter names are removed
               from the root-hub registry keys, the failed hubs are restarted, and the rest of usbip-win2 is
               uninstalled with its own uninstaller.
    status     prints the hubs and any persisted usbip2_filter state; changes nothing.
    cleanup    removes persisted usbip2_filter names from the root-hub keys (recovery, run as administrator).

  Exit codes: 0 healthy, 3010 a restart is needed, 102 problem found and rolled back (hubs healthy again),
  103 problem found and the rollback did not clear it (recovery steps: docs/usbip-roothub-recovery.zh.md),
  104 refused: Windows runs from a USB disk, 105 the guard could not run (the install is not attempted).

  Microsoft references (source records in docs/authority/source-index.tsv):
    ms-device-manager-error-messages-20261112 (code 19 CM_PROB_REGISTRY, code 14 CM_PROB_NEED_RESTART),
    ms-pnputil-command-syntax-20261112 (/delete-driver, /restart-device),
    usbip-win2-filter-readme-20261115 (why the filter exists, the all-hubs restart),
    usbip-win2-issue-201-20261115 (persisted filter state, boot from a USB disk),
    ms-extension-inf-20261115 (extension INF, AddFilter, FilterPosition).
##>

[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('preflight', 'snapshot', 'verify', 'status', 'cleanup')][string]$Action,
  [string]$State,
  [string]$Log,
  [int]$SettleMinSeconds = 12,
  [int]$SettleMaxSeconds = 60,
  # Test seams. The defaults are the real system.
  [string]$EnumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\ROOT_HUB30',
  [switch]$DryRun,                 # log the rollback steps, run none of them
  [int]$SimulateHubProblem = 0,    # pretend every physical root hub has this problem code during verify
  [switch]$SimulateUsbBoot         # pretend Windows runs from a USB disk during preflight
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# $PSScriptRoot is empty inside a param() default on Windows PowerShell 5.1, so the defaults are set here.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $State) { $State = Join-Path $here 'usbip-roothub-state.json' }
if (-not $Log) { $Log = Join-Path $here 'usbip-roothub-guard.log' }

$FilterName = 'usbip2_filter'
$PropertiesKey = '{a193307d-1245-46d3-be67-d8128670e09b}'
$CM_PROB_NEED_RESTART = 14
$script:Simulating = $false

function Write-GuardLog([string]$Message) {
  $line = '{0:yyyy-MM-dd HH:mm:ss} [usbip-guard] {1}' -f (Get-Date), $Message
  Write-Host $line
  try { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}

# --------------------------------------------------------------------------------------------- hubs

# Every present USB root hub with its problem code. A hub below the emulated controller (usbip-win2's own
# ROOT\USBIP_WIN2 node, or the older ROOT\USB node) is `Emulated`; the others are the physical ones.
function Get-RootHubs {
  $hubs = @()
  $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\ROOT_HUB*' })
  foreach ($device in $devices) {
    $problem = 0
    $parent = ''
    try { $problem = [int](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data } catch { }
    try { $parent = [string](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data } catch { }
    $emulated = ($parent -like 'ROOT\USBIP_WIN2\*') -or ($parent -like 'ROOT\USB\*')
    if ($script:Simulating -and $SimulateHubProblem -ne 0 -and -not $emulated) { $problem = $SimulateHubProblem }
    $hubs += [pscustomobject]@{ Id = $device.InstanceId; Status = [string]$device.Status; Problem = $problem; Emulated = $emulated }
  }
  return , $hubs
}

function Test-HubFault($Hub, $Before) {
  if ($Hub.Emulated) { return $false }
  if ($Hub.Problem -eq 0 -or $Hub.Problem -eq $CM_PROB_NEED_RESTART) { return $false }
  # a hub that already had this problem before the install is not ours to roll back
  $was = @($Before | Where-Object { $_.Id -eq $Hub.Id })
  if ($was.Count -gt 0 -and $was[0].Problem -eq $Hub.Problem) { return $false }
  return $true
}

function Get-HubFaults($Before) {
  $faults = @()
  $now = Get-RootHubs
  foreach ($hub in $now) { if (Test-HubFault $hub $Before) { $faults += $hub } }
  # a physical hub that was there and is gone is as bad as one with a problem code
  foreach ($old in @($Before | Where-Object { -not $_.Emulated })) {
    if (@($now | Where-Object { $_.Id -eq $old.Id }).Count -eq 0) {
      $faults += [pscustomobject]@{ Id = $old.Id; Status = 'Missing'; Problem = -1; Emulated = $false }
    }
  }
  return , $faults
}

# Waits until the hubs have been clean for the minimum time, or the maximum is reached; returns the faults then.
function Wait-HubsSettled($Before) {
  $start = Get-Date
  $clean = 0
  while ($true) {
    $faults = Get-HubFaults $Before
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if (@($faults).Count -eq 0) { $clean++ } else { $clean = 0 }
    if ($clean -ge 2 -and $elapsed -ge $SettleMinSeconds) { return , $faults }
    if ($elapsed -ge $SettleMaxSeconds) { return , $faults }
    Start-Sleep -Seconds 3
  }
}

function Format-Hubs($Hubs) {
  return (@($Hubs) | ForEach-Object { '{0} status={1} problem={2}{3}' -f $_.Id, $_.Status, $_.Problem, $(if ($_.Emulated) { ' (emulated)' } else { '' }) }) -join '; '
}

# --------------------------------------------------------------------------------------------- packages

# The oemNN.inf names of the driver packages whose original INF is `<base>.inf`, from the driver database, the way
# usbip-win2's own uninstaller finds them.
function Get-OemPackages([string]$InfBase) {
  $names = @()
  $root = 'HKLM:\SYSTEM\DriverDatabase\DriverPackages'
  if (-not (Test-Path -LiteralPath $root)) { return $names }
  foreach ($child in Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue) {
    if ($child.PSChildName.ToLowerInvariant().StartsWith($InfBase.ToLowerInvariant() + '.inf_')) {
      $oem = [string]$child.GetValue('')
      if ($oem -match '^oem\d+\.inf$') { $names += $oem.ToLowerInvariant() }
    }
  }
  return $names
}

function Invoke-GuardStep([string]$Description, [scriptblock]$Step) {
  if ($DryRun) { Write-GuardLog "dry-run, would: $Description"; return 0 }
  Write-GuardLog $Description
  $global:LASTEXITCODE = 0
  & $Step
  $code = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
  Write-GuardLog "  -> exit $code"
  return $code
}

# --------------------------------------------------------------------------------------------- registry

function Open-GuardKey([string]$PsPath, [bool]$Writable) {
  if ($PsPath -notmatch '^(HKLM|HKCU):\\(.*)$') { throw "unsupported registry path $PsPath" }
  $hive = if ($Matches[1] -eq 'HKLM') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
  return $hive.OpenSubKey($Matches[2], $Writable)
}

# Removes `usbip2_filter` from a multi-string value, keeping the value's type and every other entry. The value
# is REG_MULTI_SZ (Filters\*Upper) or binary UTF-16 multi-string data (the persisted property of issue #201).
# Returns 'removed', 'absent' or 'failed'.
function Remove-FilterNameFromValue([Microsoft.Win32.RegistryKey]$Key, [string]$ValueName) {
  $kind = $Key.GetValueKind($ValueName)
  $raw = $Key.GetValue($ValueName, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
  if ($kind -eq [Microsoft.Win32.RegistryValueKind]::MultiString) {
    $entries = [string[]]$raw
    $keep = @($entries | Where-Object { $_ -ne '' -and $_.ToLowerInvariant() -ne $FilterName })
    if ($keep.Count -eq $entries.Count) { return 'absent' }
    $Key.SetValue($ValueName, [string[]]$keep, [Microsoft.Win32.RegistryValueKind]::MultiString)
    return 'removed'
  }
  if ($kind -eq [Microsoft.Win32.RegistryValueKind]::Binary) {
    $text = [System.Text.Encoding]::Unicode.GetString([byte[]]$raw)
    $entries = @($text.Split([char]0) | Where-Object { $_ -ne '' })
    $keep = @($entries | Where-Object { $_.ToLowerInvariant() -ne $FilterName })
    if ($keep.Count -eq $entries.Count) { return 'absent' }
    $joined = ($keep -join [string][char]0) + [string][char]0 + [string][char]0
    $Key.SetValue($ValueName, [System.Text.Encoding]::Unicode.GetBytes($joined), [Microsoft.Win32.RegistryValueKind]::Binary)
    return 'removed'
  }
  return 'absent'
}

# Every place under the root-hub Enum keys that still names the filter. Each item: Path, Value.
function Find-FilterResidue {
  $found = @()
  if (-not (Test-Path -LiteralPath $EnumRoot)) { return $found }
  foreach ($hub in Get-ChildItem -LiteralPath $EnumRoot -ErrorAction SilentlyContinue) {
    $targets = @(
      @{ Path = "$EnumRoot\$($hub.PSChildName)\Filters"; Value = $null },
      @{ Path = "$EnumRoot\$($hub.PSChildName)\Properties\$PropertiesKey\0002"; Value = '' }
    )
    foreach ($target in $targets) {
      $key = $null
      try {
        $key = Open-GuardKey $target.Path $false
        if ($null -eq $key) { continue }
        $names = if ($null -eq $target.Value) { @($key.GetValueNames() | Where-Object { $_ -ne '' }) } else { @($target.Value) }
        foreach ($name in $names) {
          $text = ''
          try {
            $raw = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $text = if ($raw -is [byte[]]) { [System.Text.Encoding]::Unicode.GetString($raw) } else { ($raw -join "`0") }
          } catch { }
          if ($text.ToLowerInvariant().Contains($FilterName)) { $found += [pscustomobject]@{ Path = $target.Path; Value = $name } }
        }
      } catch { }
      finally { if ($null -ne $key) { $key.Dispose() } }
    }
  }
  return $found
}

# Returns the residue that could not be removed.
function Clear-FilterResidue {
  $left = @()
  foreach ($item in @(Find-FilterResidue)) {
    if ($DryRun) { Write-GuardLog "dry-run, would remove '$FilterName' from $($item.Path) [$($item.Value)]"; continue }
    $key = $null
    try {
      $key = Open-GuardKey $item.Path $true
      $result = Remove-FilterNameFromValue $key $item.Value
      Write-GuardLog "persisted filter name in $($item.Path) [$($item.Value)]: $result"
    } catch {
      Write-GuardLog "cannot clean $($item.Path) [$($item.Value)]: $($_.Exception.Message)"
      $left += $item
    } finally { if ($null -ne $key) { $key.Dispose() } }
  }
  return $left
}

# --------------------------------------------------------------------------------------------- rollback

function Get-UsbipUninstaller {
  foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    foreach ($entry in Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue) {
      $name = [string]$entry.GetValue('DisplayName')
      if ($name -like 'USBip*') {
        $command = [string]$entry.GetValue('UninstallString')
        if ($command -match '^"([^"]+)"') { return $Matches[1] }
        if ($command) { return $command }
      }
    }
  }
  return $null
}

function Restart-HubFaults($Faults) {
  foreach ($hub in @($Faults)) {
    if ($hub.Status -eq 'Missing') { continue }
    [void](Invoke-GuardStep "pnputil /restart-device $($hub.Id)" { & "$env:SystemRoot\System32\pnputil.exe" /restart-device $hub.Id | ForEach-Object { Write-GuardLog "  $_" } })
  }
}

function Invoke-Rollback($Snapshot, $Faults) {
  Write-GuardLog "ROLLBACK: physical root hub fault after the usbip-win2 install: $(Format-Hubs $Faults)"
  $before = @($Snapshot.filterPackages)
  $packages = @(@(Get-OemPackages $FilterName) | Where-Object { $before -notcontains $_ })
  if ($packages.Count -eq 0) { $packages = @(Get-OemPackages $FilterName) }
  foreach ($oem in $packages) {
    [void](Invoke-GuardStep "pnputil /delete-driver $oem /uninstall /force" { & "$env:SystemRoot\System32\pnputil.exe" /delete-driver $oem /uninstall /force | ForEach-Object { Write-GuardLog "  $_" } })
  }
  $residue = @(Clear-FilterResidue)
  Restart-HubFaults $Faults
  if ($DryRun) { return @{ Faults = @(); Residue = @() } }
  $script:Simulating = $false
  $faults = Wait-HubsSettled $Snapshot.hubs
  if (@($faults).Count -eq 0) {
    # The hubs are back. Leave no half install behind: usbip-win2's own uninstaller removes the UDE driver and its
    # controller node too, and finds the filter again if the first step missed it.
    $uninstaller = Get-UsbipUninstaller
    if ($uninstaller -and (Test-Path -LiteralPath $uninstaller)) {
      [void](Invoke-GuardStep "$uninstaller /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" {
          $p = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
          $global:LASTEXITCODE = $p.ExitCode
        })
      $residue += @(Clear-FilterResidue)
      $faults = Wait-HubsSettled $Snapshot.hubs
    } else {
      Write-GuardLog 'no usbip-win2 uninstall entry found; the UDE driver stays'
    }
  }
  $residue = @($residue) + @(Find-FilterResidue)
  return @{ Faults = @($faults); Residue = @($residue) }
}

# --------------------------------------------------------------------------------------------- actions

function Test-SystemOnUsbDisk {
  if ($SimulateUsbBoot) { return $true }
  try {
    $letter = $env:SystemDrive.TrimEnd(':')
    $disk = Get-Partition -DriveLetter $letter -ErrorAction Stop | Get-Disk -ErrorAction Stop
    return ([string]$disk.BusType -eq 'USB')
  } catch { return $false }
}

function Invoke-Preflight {
  if (Test-SystemOnUsbDisk) {
    Write-GuardLog 'REFUSED: Windows runs from a USB disk. The usbip-win2 filter restarts every USB root hub, which can stop such a system booting (usbip-win2 issue #201). The USB/IP component is not installed.'
    return 104
  }
  $hubs = Get-RootHubs
  Write-GuardLog "preflight ok. root hubs: $(Format-Hubs $hubs)"
  return 0
}

function Invoke-Snapshot {
  $hubs = Get-RootHubs
  $snapshot = [pscustomobject]@{
    time           = (Get-Date).ToString('o')
    hubs           = @($hubs)
    filterPackages = @(Get-OemPackages $FilterName)
    udePackages    = @(Get-OemPackages 'usbip2_ude')
  }
  $snapshot | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $State -Encoding UTF8
  Write-GuardLog "snapshot: root hubs: $(Format-Hubs $hubs); filter packages: $(@($snapshot.filterPackages) -join ',')"
  return 0
}

function Invoke-Verify {
  if (-not (Test-Path -LiteralPath $State)) { throw "no snapshot at $State" }
  $snapshot = Get-Content -LiteralPath $State -Raw | ConvertFrom-Json
  $snapshot.hubs = @($snapshot.hubs)
  $script:Simulating = $true
  $faults = Wait-HubsSettled $snapshot.hubs
  $now = Get-RootHubs
  if (@($faults).Count -eq 0) {
    $restart = @($now | Where-Object { $_.Problem -eq $CM_PROB_NEED_RESTART -and -not $_.Emulated })
    Write-GuardLog "verify ok. root hubs: $(Format-Hubs $now)"
    if ($restart.Count -gt 0) { Write-GuardLog 'a root hub asks for a restart (code 14); the installer reports it'; return 3010 }
    return 0
  }
  $result = Invoke-Rollback $snapshot $faults
  if (@($result.Faults).Count -eq 0 -and @($result.Residue).Count -eq 0) {
    if ($DryRun) { Write-GuardLog 'ROLLBACK DONE (dry run: nothing was changed)' } else { Write-GuardLog "ROLLBACK DONE: root hubs healthy again: $(Format-Hubs (Get-RootHubs))" }
    return 102
  }
  Write-GuardLog "ROLLBACK INCOMPLETE: hubs: $(Format-Hubs $result.Faults); persisted filter state: $(@($result.Residue | ForEach-Object { $_.Path }) -join ', '). Follow docs/usbip-roothub-recovery.zh.md."
  return 103
}

function Invoke-Cleanup {
  $left = @(Clear-FilterResidue)
  $remaining = @(Find-FilterResidue)
  Write-GuardLog "cleanup: $($remaining.Count) persisted filter name(s) left"
  if ($left.Count -gt 0 -or $remaining.Count -gt 0) { return 103 }
  return 0
}

function Invoke-Status {
  Write-GuardLog "root hubs: $(Format-Hubs (Get-RootHubs))"
  Write-GuardLog "filter packages: $(@(Get-OemPackages $FilterName) -join ',') ; ude packages: $(@(Get-OemPackages 'usbip2_ude') -join ',')"
  $residue = @(Find-FilterResidue)
  Write-GuardLog "persisted filter names: $($residue.Count)$(if ($residue.Count) { ' -> ' + (($residue | ForEach-Object { $_.Path }) -join ', ') })"
  return 0
}

try {
  $rc = switch ($Action) {
    'preflight' { Invoke-Preflight }
    'snapshot' { Invoke-Snapshot }
    'verify' { Invoke-Verify }
    'status' { Invoke-Status }
    'cleanup' { Invoke-Cleanup }
  }
  exit ([int]($rc | Select-Object -Last 1))
} catch {
  Write-GuardLog "guard error in ${Action}: $($_.Exception.Message)"
  exit 105
}
