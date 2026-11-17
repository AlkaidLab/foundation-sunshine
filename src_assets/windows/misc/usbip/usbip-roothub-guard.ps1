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
  # The slim package supplies its own driver/tool cleanup instead of an Inno uninstaller.
  [string]$RollbackScript,
  [int]$SettleMinSeconds = 12,
  [int]$SettleMaxSeconds = 60,
  # Test seams. The defaults are the real system.
  [string]$EnumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\ROOT_HUB30',
  [switch]$DryRun,                 # log the rollback steps, run none of them
  [int]$SimulateHubProblem = 0,    # pretend every physical root hub has this problem code during verify
  [switch]$SimulateUsbBoot,        # pretend Windows runs from a USB disk during preflight
  [switch]$SimulateHubMissing,     # during verify, hide the physical hubs: they are gone after the install
  [switch]$SimulateNoHubs,         # pretend the machine has no USB root hub at all (a hosted runner, a VM)
  [switch]$SimulateAllHubsPhysical, # treat every root hub as physical (a machine with only virtual hubs can then run the fault cases)
  [switch]$SimulateUnreadable,     # pretend the problem code of every physical root hub cannot be read
  [switch]$SimulateDiskQueryFailure # pretend the system-disk query fails during preflight
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
$script:PartFailures = @()
$script:ScratchEnum = $EnumRoot -notlike 'HKLM:\SYSTEM\CurrentControlSet\Enum\*'

function Write-GuardLog([string]$Message) {
  $line = '{0:yyyy-MM-dd HH:mm:ss} [usbip-guard] {1}' -f (Get-Date), $Message
  Write-Host $line
  try { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
}

# --------------------------------------------------------------------------------------------- hubs

# A property of an object that may not have it (an older snapshot, a fault made by hand): the default instead of a StrictMode error.
function Get-Field($Object, [string]$Name, $Default) {
  if ($null -eq $Object) { return $Default }
  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) { return $Default }
  return $property.Value
}

# The text of a device property for matching: strings as they are, binary data (REG_BINARY) decoded as UTF-16 and as 8-bit text.
# Joining a byte array with '|' gives decimal numbers that never match a name.
function ConvertTo-PropertyText($Data) {
  if ($null -eq $Data) { return '' }
  if ($Data -is [byte[]]) {
    return ([System.Text.Encoding]::Unicode.GetString($Data) + '|' + [System.Text.Encoding]::UTF8.GetString($Data)).Replace([string][char]0, '|')
  }
  $parts = @()
  foreach ($item in @($Data)) { $parts += $(if ($item -is [byte[]]) { ConvertTo-PropertyText $item } else { [string]$item }) }
  return ($parts -join '|')
}

# Every present USB root hub with its problem code. A hub below a root-enumerated controller (usbip-win2's ROOT\ALKAIDUSB or
# ROOT\USBIP_WIN2, another virtual host controller's ROOT\USB) is `Emulated`; the physical ones sit below PCI, ACPI, ...
# Fail closed: a hub whose problem code cannot be read has Problem -2 (`Unreadable`), never 0; a failed enumeration throws
# (the guard then exits 105). A parent that cannot be read leaves the hub "physical", the cautious side.
# An empty list is a valid answer (a VM or a hosted runner can have no USB root hub). The result is returned as a plain list, which
# PowerShell unrolls: with no hub the caller gets nothing, so every caller wraps the call in @(...) and never indexes or reads a
# property of "the list" itself (the first version returned `, $hubs`, whose empty array became ONE element: `.Emulated` of it
# threw under StrictMode on a machine without root hubs).
function Get-RootHubs {
  $hubs = @()
  if ($SimulateNoHubs) { return $hubs }
  $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like 'USB\ROOT_HUB*' })
  foreach ($device in $devices) {
    $parent = ''
    try { $parent = [string](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data } catch { }
    $emulated = ($parent -like 'ROOT\*') -and -not $SimulateAllHubsPhysical
    if ($script:Simulating -and $SimulateHubMissing -and -not $emulated) { continue }
    $problem = 0
    $unreadable = $false
    try {
      if ($SimulateUnreadable -and -not $emulated) { throw 'simulated: the problem code cannot be read' }
      $problem = [int](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data
    } catch { $problem = -2; $unreadable = $true }
    if ($script:Simulating -and $SimulateHubProblem -ne 0 -and -not $emulated -and -not $unreadable) { $problem = $SimulateHubProblem }
    $hubs += [pscustomobject]@{ Id = $device.InstanceId; Status = [string]$device.Status; Problem = $problem; Emulated = $emulated; Unreadable = $unreadable }
  }
  return $hubs
}

# The physical hubs whose problem code cannot be read: nothing can be said about them, so nothing may go on.
function Assert-HubsReadable($Hubs) {
  $bad = @(@($Hubs) | Where-Object { $_.Unreadable -and -not $_.Emulated })
  if ($bad.Count -gt 0) { throw "the problem code of a physical root hub cannot be read: $(($bad | ForEach-Object { $_.Id }) -join ', ')" }
}

function Test-HubFault($Hub, $Before) {
  if ($Hub.Emulated) { return $false }
  if ($Hub.Unreadable) { return $true }   # unknown is not healthy
  if ($Hub.Problem -eq 0 -or $Hub.Problem -eq $CM_PROB_NEED_RESTART) { return $false }
  # a hub that already had this problem before the install is not ours to roll back
  $was = @($Before | Where-Object { $_.Id -eq $Hub.Id })
  if ($was.Count -gt 0 -and $was[0].Problem -eq $Hub.Problem) { return $false }
  return $true
}

function Get-HubFaults($Before) {
  $faults = @()
  $now = @(Get-RootHubs)
  foreach ($hub in $now) { if (Test-HubFault $hub $Before) { $faults += $hub } }
  # a physical hub that was there and is gone is as bad as one with a problem code
  foreach ($old in @($Before | Where-Object { $null -ne $_ -and -not (Get-Field $_ 'Emulated' $false) })) {
    if (@($now | Where-Object { $_.Id -eq $old.Id }).Count -eq 0) {
      # the same shape as the objects of Get-RootHubs: a fault object without a property used to break Format-Hubs under StrictMode
      $faults += [pscustomobject]@{ Id = $old.Id; Status = 'Missing'; Problem = -1; Emulated = $false; Unreadable = $false }
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

# Never throws: a log line must not be able to stop a recovery step (it is built before the steps run).
function Format-Hubs($Hubs) {
  try {
    $lines = foreach ($hub in @($Hubs | Where-Object { $null -ne $_ })) {
      try {
        '{0} status={1} problem={2}{3}{4}' -f (Get-Field $hub 'Id' '?'), (Get-Field $hub 'Status' '?'), (Get-Field $hub 'Problem' '?'),
          $(if (Get-Field $hub 'Emulated' $false) { ' (emulated)' } else { '' }), $(if (Get-Field $hub 'Unreadable' $false) { ' (problem code unreadable)' } else { '' })
      } catch { '(a hub that cannot be printed)' }
    }
    return (@($lines) -join '; ')
  } catch { return '(the hubs cannot be printed)' }
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
  # A step may print (a child script, a native tool). Printed lines must not become part of this function's
  # result: the caller compares the result with 0, and an array of output lines plus 0 made a successful removal
  # of the slim package count as a failed step, so a clean rollback was reported as incomplete (103).
  try { & $Step | Out-Host } catch { Write-GuardLog "  step failed: $($_.Exception.Message)"; return 1 }
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

# Whether any device property of a hub names the filter (readable without registry rights: the extension INF leaves it in
# {A193307D-1245-46D3-BE67-D8128670E09B} 2). Throws when the properties cannot be read.
function Test-HubPropertiesNameFilter([string]$InstanceId) {
  foreach ($property in @(Get-PnpDeviceProperty -InstanceId $InstanceId -ErrorAction Stop)) {
    $text = try { ConvertTo-PropertyText $property.Data } catch { '' }
    if ($text.ToLowerInvariant().Contains($FilterName)) { return $true }
  }
  return $false
}

# Every place under the root-hub Enum keys that still names the filter. Each item: Path, Value, and Unreadable when the place
# could not be read at all (fail closed: an unreadable key is reported, never taken for "no residue"). A key the registry
# refuses is looked at through the hub's device properties instead; only when that fails too is it Unreadable.
function Find-FilterResidue {
  $found = @()
  if (-not (Test-Path -LiteralPath $EnumRoot)) { return $found }
  # The present physical hubs are the ones whose filter state matters (they carry the keyboard and mouse). The filter on a virtual
  # hub is the point of the install on a machine that has usbip-win2, and a hub that is not present has nothing to restart.
  $physicalIds = @(@(Get-RootHubs) | Where-Object { -not $_.Emulated } | ForEach-Object { $_.Id.ToLowerInvariant() })
  foreach ($hub in Get-ChildItem -LiteralPath $EnumRoot -ErrorAction Stop) {
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
          $raw = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
          $text = if ($raw -is [byte[]]) { [System.Text.Encoding]::Unicode.GetString($raw) } else { ($raw -join "`0") }
          if ($text.ToLowerInvariant().Contains($FilterName)) { $found += [pscustomobject]@{ Path = $target.Path; Value = $name; Unreadable = $false } }
        }
      } catch {
        $why = $_.Exception.Message
        $instance = "USB\ROOT_HUB30\$($hub.PSChildName)"
        # a key the registry refuses is only looked at further for a present physical hub
        if ($physicalIds -notcontains $instance.ToLowerInvariant() -and -not $script:ScratchEnum) { continue }
        try {
          if (Test-HubPropertiesNameFilter $instance) { $found += [pscustomobject]@{ Path = $target.Path; Value = '(device property)'; Unreadable = $false } }
        } catch {
          Write-GuardLog "cannot read $($target.Path): $why"
          $found += [pscustomobject]@{ Path = $target.Path; Value = '(unreadable)'; Unreadable = $true }
        }
      }
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
    if ($item.Unreadable) { $left += $item; continue }   # cannot be cleaned what cannot be read
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
    # a failed restart is logged; whether it matters shows in the hub state that Wait-HubsSettled checks afterwards
    [void](Invoke-GuardStep "pnputil /restart-device $($hub.Id)" { & "$env:SystemRoot\System32\pnputil.exe" /restart-device $hub.Id | ForEach-Object { Write-GuardLog "  $_" } })
  }
}

# One part of the rollback. An error in it (a registry read, a log line, a query) is kept as a failure of that part and never stops the parts
# after it: the recovery steps run whatever happened before. $Default is what the part gives when it failed.
function Invoke-RollbackPart([string]$Name, [scriptblock]$Part, $Default) {
  try { return (& $Part) } catch {
    $script:PartFailures += "${Name}: $($_.Exception.Message)"
    try { Write-GuardLog "rollback part failed, going on: ${Name}: $($_.Exception.Message)" } catch { }
    return $Default
  }
}

function Invoke-Rollback($Snapshot, $Faults) {
  $script:PartFailures = @()
  try { Write-GuardLog "ROLLBACK: physical root hub fault after the usbip-win2 install: $(Format-Hubs $Faults)" } catch { }
  $packages = @(Invoke-RollbackPart 'list the filter packages' {
      $before = @($Snapshot.filterPackages)
      $found = @(@(Get-OemPackages $FilterName) | Where-Object { $before -notcontains $_ })
      if ($found.Count -eq 0) { $found = @(Get-OemPackages $FilterName) }
      $found
    } @())
  # The result of every step that has to succeed is kept: 102 ("rolled back") is only reported when none of them failed.
  # 0 and 3010 (restart needed) are success.
  $stepFailures = @()
  foreach ($oem in $packages) {
    $rc = Invoke-GuardStep "pnputil /delete-driver $oem /uninstall /force" { & "$env:SystemRoot\System32\pnputil.exe" /delete-driver $oem /uninstall /force | ForEach-Object { Write-GuardLog "  $_" } }
    if ($rc -ne 0 -and $rc -ne 3010) { $stepFailures += "pnputil /delete-driver $oem exit $rc" }
  }
  $residue = @(Invoke-RollbackPart 'clear the persisted filter names' { Clear-FilterResidue } @())
  [void](Invoke-RollbackPart 'restart the faulty hubs' { Restart-HubFaults $Faults } $null)
  if ($DryRun) { return @{ Faults = @(); Residue = @(); StepFailures = @($stepFailures) + @($script:PartFailures) } }
  $script:Simulating = $false
  # if the hub state cannot be read, that is a hub fault that is still there, not a clean state
  $unknown = @([pscustomobject]@{ Id = '(hub state)'; Status = 'Unknown'; Problem = -2; Emulated = $false; Unreadable = $true })
  $faults = @(Invoke-RollbackPart 'wait for the hubs' { Wait-HubsSettled $Snapshot.hubs } $unknown)
  if ($faults.Count -eq 0) {
    # The hubs are back. Leave no half install behind: usbip-win2's own uninstaller removes the UDE driver and its
    # controller node too, and finds the filter again if the first step missed it.
    $uninstaller = Invoke-RollbackPart 'find the uninstaller' { Get-UsbipUninstaller } $null
    if ($RollbackScript -and (Test-Path -LiteralPath $RollbackScript)) {
      $rc = Invoke-GuardStep 'remove the slim USB/IP package' {
          & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $RollbackScript -Action remove -InstallRoot (Split-Path -Parent $RollbackScript)
        }
      if ($rc -ne 0 -and $rc -ne 3010) { $stepFailures += "USB/IP package cleanup exit $rc" }
      $residue += @(Invoke-RollbackPart 'clear the persisted filter names (after package cleanup)' { Clear-FilterResidue } @())
      $faults = @(Invoke-RollbackPart 'wait for the hubs (after package cleanup)' { Wait-HubsSettled $Snapshot.hubs } $unknown)
    } elseif ($uninstaller -and (Test-Path -LiteralPath $uninstaller)) {
      $rc = Invoke-GuardStep "$uninstaller /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" {
          $p = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
          $global:LASTEXITCODE = $p.ExitCode
        }
      if ($rc -ne 0 -and $rc -ne 3010) { $stepFailures += "usbip-win2 uninstaller exit $rc" }
      $residue += @(Invoke-RollbackPart 'clear the persisted filter names (after the uninstaller)' { Clear-FilterResidue } @())
      $faults = @(Invoke-RollbackPart 'wait for the hubs (after the uninstaller)' { Wait-HubsSettled $Snapshot.hubs } $unknown)
    } else {
      Write-GuardLog 'no usbip-win2 uninstall entry found; the UDE driver stays'
    }
  }
  $residue = @($residue) + @(Invoke-RollbackPart 'scan for persisted filter names' { Find-FilterResidue } @([pscustomobject]@{ Path = '(registry scan)'; Value = '(unreadable)'; Unreadable = $true }))
  return @{ Faults = @($faults); Residue = @($residue); StepFailures = @($stepFailures) + @($script:PartFailures) }
}

# --------------------------------------------------------------------------------------------- actions

function Test-SystemOnUsbDisk {
  if ($SimulateUsbBoot) { return $true }
  # Fail closed: when the boot medium cannot be determined, the install does not go on (the exception ends the guard with 105).
  if ($SimulateDiskQueryFailure) { throw 'simulated: the system disk cannot be queried' }
  if (-not $env:SystemDrive) { throw 'SystemDrive is not set' }
  $letter = $env:SystemDrive.TrimEnd(':')
  $partitions = @(Get-Partition -DriveLetter $letter -ErrorAction Stop)
  if ($partitions.Count -eq 0) { throw "no partition found for the system drive $letter" }
  $disk = $partitions[0] | Get-Disk -ErrorAction Stop
  $bus = [string]$disk.BusType
  if ([string]::IsNullOrWhiteSpace($bus) -or $bus -eq 'Unknown') { throw "the bus type of the system disk is not recognised ('$bus')" }
  return ($bus -eq 'USB')
}

function Invoke-Preflight {
  if (Test-SystemOnUsbDisk) {
    Write-GuardLog 'REFUSED: Windows runs from a USB disk. The usbip-win2 filter restarts every USB root hub, which can stop such a system booting (usbip-win2 issue #201). The USB/IP component is not installed.'
    return 104
  }
  $hubs = @(Get-RootHubs)
  Assert-HubsReadable $hubs
  Write-GuardLog "preflight ok. root hubs: $(Format-Hubs $hubs)"
  return 0
}

function Invoke-Snapshot {
  $hubs = @(Get-RootHubs)
  Assert-HubsReadable $hubs
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
  # a snapshot of a machine without root hubs has an empty (or null) list: no hub, not one null hub
  $snapshot.hubs = @($snapshot.hubs | Where-Object { $null -ne $_ })
  $script:Simulating = $true
  $faults = Wait-HubsSettled $snapshot.hubs
  $now = @(Get-RootHubs)
  if (@($faults).Count -eq 0) {
    $restart = @($now | Where-Object { $_.Problem -eq $CM_PROB_NEED_RESTART -and -not $_.Emulated })
    Write-GuardLog "verify ok. root hubs: $(Format-Hubs $now)"
    if ($restart.Count -gt 0) { Write-GuardLog 'a root hub asks for a restart (code 14); the installer reports it'; return 3010 }
    return 0
  }
  $result = Invoke-Rollback $snapshot $faults
  if (@($result.Faults).Count -eq 0 -and @($result.Residue).Count -eq 0 -and @($result.StepFailures).Count -eq 0) {
    if ($DryRun) { Write-GuardLog 'ROLLBACK DONE (dry run: nothing was changed)' } else { Write-GuardLog "ROLLBACK DONE: root hubs healthy again: $(Format-Hubs @(Get-RootHubs))" }
    return 102
  }
  Write-GuardLog "ROLLBACK INCOMPLETE: hubs: $(Format-Hubs $result.Faults); persisted filter state: $(@($result.Residue | ForEach-Object { $_.Path }) -join ', '); failed steps: $(@($result.StepFailures) -join '; '). The pre-install snapshot is kept at $State. Follow docs/usbip-roothub-recovery.zh.md."
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
  Write-GuardLog "root hubs: $(Format-Hubs @(Get-RootHubs))"
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
