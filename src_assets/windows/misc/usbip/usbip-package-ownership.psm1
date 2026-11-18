#requires -version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:UsbipUninstallBases = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')
$script:UsbipSourceHash = '38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'
# What the earlier release installer asked the upstream setup for: a silent run with every default component.
$script:UsbipDefaultComponents = 'main,client,gui,sdk,pdb'
# The guard log stamps whole seconds, the file system 100 ns ticks: allow a little slack on both ends of a window.
$script:UsbipWindowSlack = [TimeSpan]::FromSeconds(2)

# Inno Setup writes InstallLocation with a trailing backslash (observed with usbip-win2 0.9.8.1),
# so a plain string comparison against the default root never matches. Windows paths are case-insensitive.
function Test-UsbipSameLocation([string]$Left, [string]$Right) {
  if (-not $Left -or -not $Right) { return $false }
  try {
    $a = [IO.Path]::GetFullPath($Left).TrimEnd([char]92)
    $b = [IO.Path]::GetFullPath($Right).TrimEnd([char]92)
    return [string]::Equals($a, $b, [StringComparison]::OrdinalIgnoreCase)
  } catch { return $false }
}

# The takeover runs the registered uninstaller with elevated rights, so it must be the one in the install directory.
function Test-UsbipUninstallerInRoot($Entry, [string]$Root) {
  $command = [string](Get-UsbipValue $Entry 'UninstallString')
  if ($command -notmatch '^"([^"]+)\\unins[0-9]+\.exe"(?:\s+/LOG)?$') { return $false }
  return Test-UsbipSameLocation $Matches[1] $Root
}

function Get-UsbipValue($Record, [string]$Name) {
  if ($null -ne $Record -and $null -ne $Record.PSObject.Properties[$Name]) { return $Record.$Name }
  return ''
}

# Test seam: creation and last-write time of one file (usbip-legacy-dotnet-creationtime-20261118). Inno Setup creates
# every file it installs, so the creation time is when the upstream setup ran (observed on a real machine: all files
# of one setup share one creation second).
function Get-UsbipFileTimes([string]$Path) {
  $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
  [pscustomobject]@{ Created = $item.CreationTime; Written = $item.LastWriteTime }
}

# The root-hub guard of the earlier release installer logged one transaction around every run of the upstream setup:
#   <time> [usbip-guard] snapshot: root hubs: ...; filter packages: <empty>      (before: no usbip filter package yet)
#   <time> [usbip-guard] verify ok. root hubs: ...                                (after the setup, hubs healthy)
# A run that found the same version already installed never reached the guard, so these two lines exist only when
# that installer really ran the upstream setup. A window counts only when the filter list was empty (nothing of
# usbip-win2 existed before) and no rollback, refusal or error line sits between the two.
function Get-UsbipInstallWindows([string]$LogPath) {
  $windows = @()
  if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf -ErrorAction Stop)) { return $windows }
  $start = $null
  foreach ($line in [IO.File]::ReadAllLines($LogPath)) {
    if ($line -notmatch '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) \[usbip-guard\] (.*)$') { continue }
    $time = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture)
    $message = $Matches[2]
    if ($message -match '^snapshot: .*; filter packages: *$') { $start = $time }
    elseif ($message -match '^(snapshot|preflight|cleanup|ROLLBACK|REFUSED|guard error)') { $start = $null }
    elseif ($message -match '^verify ok\.' -and $null -ne $start) {
      $windows += [pscustomobject]@{ Start = $start; End = $time }
      $start = $null
    }
  }
  return $windows
}

# Which signals show that the earlier release installer ran the upstream setup. Every one must hold; a missing or
# contradicting one means "not proven" and the install stays untouched. The upstream uninstall entry alone cannot
# decide it: a user's own default install writes the same values (UninstallString with /LOG, components, tasks;
# usbip-legacy-inno-cmdline-20261118, usbip-legacy-inno-setuplogging-20261118).
function Get-UsbipFsInstallProof($Entries, $Receipt, [string]$Root, [bool]$BundledInstaller, [string]$GuardLog) {
  $checks = [ordered]@{}
  $entry = if (@($Entries).Count -eq 1) { @($Entries)[0] } else { $null }
  # 1 exactly one upstream registration and no receipt of ours
  $checks['single-registration'] = $null -ne $entry -and $null -eq $Receipt
  # 2 the host still carries the pinned upstream installer
  $checks['bundled-installer'] = $BundledInstaller
  $window = $null
  if ($null -ne $entry) {
    # 3 a silent default run: the full component list, none deselected
    $checks['default-components'] = (([string](Get-UsbipValue $entry 'Inno Setup: Selected Components')) -ceq $script:UsbipDefaultComponents) -and
      (-not [string](Get-UsbipValue $entry 'Inno Setup: Deselected Components'))
    # 4 one setup only: a single uninstaller, registered, inside the install directory
    $uninstallers = @()
    $inRoot = (Test-UsbipSameLocation (Get-UsbipValue $entry 'InstallLocation') $Root) -and (Test-UsbipUninstallerInRoot $entry $Root)
    if ($inRoot -and (Test-Path -LiteralPath $Root -PathType Container -ErrorAction Stop)) {
      $uninstallers = @(Get-ChildItem -LiteralPath $Root -Filter 'unins*.exe' -File -Force -ErrorAction Stop)
    }
    $checks['single-uninstaller'] = $inRoot -and $uninstallers.Count -eq 1
    # 5 the setup ran inside a guard transaction of the earlier installer: every installed file was created and the
    # uninstall data was last written between the transaction's two log lines
    if ($checks['single-uninstaller']) {
      $exe = $uninstallers[0].FullName
      $dat = [IO.Path]::ChangeExtension($exe, '.dat')
      $files = @('usbip.exe', 'devnode.exe', 'libusbip.dll', 'wusbip.exe' | ForEach-Object { Join-Path $Root $_ }) + @($exe, $dat)
      try {
        $created = @($files | ForEach-Object { (Get-UsbipFileTimes $_).Created })
        $written = (Get-UsbipFileTimes $dat).Written
        foreach ($candidate in @(Get-UsbipInstallWindows $GuardLog)) {
          $from = $candidate.Start - $script:UsbipWindowSlack
          $to = $candidate.End + $script:UsbipWindowSlack
          if (@($created | Where-Object { $_ -lt $from -or $_ -gt $to }).Count -eq 0 -and $written -ge $from -and $written -le $to) { $window = $candidate }
        }
      } catch { $window = $null }
    }
    $checks['inside-guard-window'] = $null -ne $window
    # 6 the registered install day is the day of that transaction
    $date = [string](Get-UsbipValue $entry 'InstallDate')
    $checks['install-date'] = $null -ne $window -and (($date -ceq $window.Start.ToString('yyyyMMdd')) -or ($date -ceq $window.End.ToString('yyyyMMdd')))
  }
  $names = @($checks.Keys)
  [pscustomobject]@{
    Proven = (@($names | Where-Object { -not $checks[$_] }).Count -eq 0 -and $names.Count -eq 6)
    Signals = @($names | Where-Object { $checks[$_] })
    Missing = @($names | Where-Object { -not $checks[$_] })
  }
}

# usbip-policy-reg-20261117, ARP values: version/location are facts, not ownership.
# usbip-policy-read-20261117, LiteralPath: reads only; errors default to external.
function Get-UsbipInventory([string]$Root, [string]$HostRoot) {
  $entries = @()
  $receipt = $null
  foreach ($base in $script:UsbipUninstallBases) {
    $key = Join-Path $base '{199505b0-b93d-4521-a8c7-897818e0205a}_is1'
    if (Test-Path -LiteralPath $key -ErrorAction Stop) { $entries += Get-ItemProperty -LiteralPath $key -ErrorAction Stop }
    $key = Join-Path $base 'AlkaidLabUSBip'
    if (Test-Path -LiteralPath $key -ErrorAction Stop) {
      if ($null -ne $receipt) { throw 'Conflicting USB/IP receipts' }
      $receipt = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
    }
  }
  $locations = @($Root) + @($entries | ForEach-Object { [string](Get-UsbipValue $_ 'InstallLocation') })
  $extras = $false
  $present = $entries.Count -gt 0 -or $null -ne $receipt
  foreach ($location in $locations | Select-Object -Unique) {
    if (-not $location) { continue }
    foreach ($member in 'usbip.exe', 'payload.json', 'devnode.exe') {
      if (Test-Path -LiteralPath (Join-Path $location $member) -ErrorAction Stop) { $present = $true }
    }
    foreach ($member in 'wusbip.exe', 'include', 'lib') {
      if (Test-Path -LiteralPath (Join-Path $location $member) -ErrorAction Stop) { $extras = $true; $present = $true }
    }
  }
  foreach ($name in 'usbip2_ude', 'usbip2_filter', 'usbip2_vhci') {
    if (Test-Path -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Services\' + $name) -ErrorAction Stop) { $present = $true }
  }
  # usbip-policy-files-20261117, -LiteralPath/-Filter. A generic success journal
  # cannot prove ownership: reuse of an external install also succeeds.
  $legacyPayload = $false
  $bundled = Join-Path $HostRoot 'scripts\usbip'
  if (Test-Path -LiteralPath $bundled -ErrorAction Stop) {
    foreach ($file in @(Get-ChildItem -LiteralPath $bundled -Filter 'USBip-*.exe' -File -ErrorAction Stop)) {
      if ($file.Name -eq 'USBip-0.9.8.1-x64.exe' -and
          (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -eq $script:UsbipSourceHash) { $legacyPayload = $true }
    }
  }
  # Only a full upstream install with extras needs the stronger proof; without extras the bundled installer rule stays.
  $proof = $null
  if ($extras -and $entries.Count -gt 0) {
    $proof = Get-UsbipFsInstallProof $entries $receipt $Root $legacyPayload (Join-Path $HostRoot 'scripts\usbip-roothub-guard.log')
  }
  [pscustomobject]@{ Present = $present; Entries = $entries; Receipt = $receipt; Extras = $extras; LegacyPayload = $legacyPayload; Root = $Root; Proof = $proof }
}

# Whether a user may ask for a replacement of this install: exactly one upstream registration of the fixed AppId in
# the standard directory with its own uninstaller in place, and nothing of ours. The version does not matter here:
# the person decides, and the replacement installs the pinned release.
function Test-UsbipReplaceable($Inventory) {
  if ($null -ne $Inventory.Receipt -or $Inventory.Entries.Count -ne 1) { return $false }
  $entry = $Inventory.Entries[0]
  if (-not (Test-UsbipSameLocation (Get-UsbipValue $entry 'InstallLocation') $Inventory.Root)) { return $false }
  if (-not (Test-UsbipUninstallerInRoot $entry $Inventory.Root)) { return $false }
  $command = [string](Get-UsbipValue $entry 'UninstallString')
  if ($command -notmatch '^"([^"]+\\unins[0-9]+\.exe)"') { return $false }
  return [bool](Test-Path -LiteralPath $Matches[1] -PathType Leaf -ErrorAction Stop)
}

function Resolve-UsbipOwnership($Inventory, [switch]$ConfirmedReplace) {
  $versions = @($Inventory.Entries | ForEach-Object { [string](Get-UsbipValue $_ 'DisplayVersion') })
  if ($null -ne $Inventory.Receipt) { $versions += [string](Get-UsbipValue $Inventory.Receipt 'DisplayVersion') }
  $compatible = $versions.Count -gt 0 -and @($versions | Where-Object { $_ -cne '0.9.8.1' }).Count -eq 0
  $proven = $null -ne $Inventory.Proof -and $Inventory.Proof.Proven
  $owner = 'external'
  $reason = 'unproven-origin'
  if (-not $Inventory.Present) { $owner = 'absent'; $reason = 'not-installed' }
  elseif ($Inventory.Extras -and -not $proven) { $reason = 'gui-or-sdk' }
  elseif (-not $compatible) { $reason = 'unsupported-version' }
  elseif ($null -ne $Inventory.Receipt -and
      (Get-UsbipValue $Inventory.Receipt 'InstallLayout') -eq 'usbip-slim-v1' -and
      (Get-UsbipValue $Inventory.Receipt 'SourceSHA256') -eq $script:UsbipSourceHash -and
      (Test-UsbipSameLocation (Get-UsbipValue $Inventory.Receipt 'InstallLocation') $Inventory.Root) -and
      $Inventory.Entries.Count -eq 0) { $owner = 'managed'; $reason = 'slim-receipt' }
  # The earlier release installer ran the upstream setup with default components: GUI, SDK and PDB came with it.
  # Every proof signal must hold at once (see Get-UsbipFsInstallProof); a single missing one keeps it external.
  elseif ($proven -and $Inventory.Entries.Count -eq 1) { $owner = 'legacy-owned'; $reason = 'fs-installed-proof' }
  # Exactly one upstream registration: two records (one per registry view) leave the origin ambiguous, and the
  # takeover would run each uninstaller in turn.
  elseif ($Inventory.Entries.Count -eq 1 -and $null -eq $Inventory.Receipt -and $Inventory.LegacyPayload -and
      @($Inventory.Entries | Where-Object { -not ((Test-UsbipSameLocation (Get-UsbipValue $_ 'InstallLocation') $Inventory.Root) -and (Test-UsbipUninstallerInRoot $_ $Inventory.Root)) }).Count -eq 0) {
    $owner = 'legacy-owned'; $reason = 'same-version-no-extras-bundled-installer'
  }
  $replaceable = $owner -eq 'external' -and (Test-UsbipReplaceable $Inventory)
  # The person asked for this replacement in the panel and confirmed it: the one external case that may be taken over.
  if ($ConfirmedReplace -and $replaceable) { $owner = 'legacy-owned'; $reason = 'user-confirmed-replace' }
  $version = if ($Inventory.Entries.Count -gt 0) { [string](Get-UsbipValue $Inventory.Entries[0] 'DisplayVersion') } else { '' }
  [pscustomobject]@{
    ownership = $owner; compatible = $compatible; reason = $reason; replaceable = $replaceable; version = $version
    proofMissing = $(if ($null -ne $Inventory.Proof) { @($Inventory.Proof.Missing) } else { @() })
  }
}

function Get-UsbipOwnership([string]$Root, [string]$HostRoot, [switch]$ConfirmedReplace) {
  try { Resolve-UsbipOwnership (Get-UsbipInventory $Root $HostRoot) -ConfirmedReplace:$ConfirmedReplace }
  catch { [pscustomobject]@{ ownership = 'external'; compatible = $false; reason = 'inspection-failed'; replaceable = $false; version = ''; proofMissing = @() } }
}
Export-ModuleMember -Function *-Usbip*
