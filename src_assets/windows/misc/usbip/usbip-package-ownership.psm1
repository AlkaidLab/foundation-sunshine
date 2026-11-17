#requires -version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:UsbipUninstallBases = @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')
$script:UsbipSourceHash = '38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'

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

function Get-UsbipValue($Record, [string]$Name) {
  if ($null -ne $Record -and $null -ne $Record.PSObject.Properties[$Name]) { return $Record.$Name }
  return ''
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
  [pscustomobject]@{ Present = $present; Entries = $entries; Receipt = $receipt; Extras = $extras; LegacyPayload = $legacyPayload; Root = $Root }
}

function Resolve-UsbipOwnership($Inventory) {
  $versions = @($Inventory.Entries | ForEach-Object { [string](Get-UsbipValue $_ 'DisplayVersion') })
  if ($null -ne $Inventory.Receipt) { $versions += [string](Get-UsbipValue $Inventory.Receipt 'DisplayVersion') }
  $compatible = $versions.Count -gt 0 -and @($versions | Where-Object { $_ -cne '0.9.8.1' }).Count -eq 0
  $owner = 'external'
  $reason = 'unproven-origin'
  if (-not $Inventory.Present) { $owner = 'absent'; $reason = 'not-installed' }
  elseif ($Inventory.Extras) { $reason = 'gui-or-sdk' }
  elseif (-not $compatible) { $reason = 'unsupported-version' }
  elseif ($null -ne $Inventory.Receipt -and
      (Get-UsbipValue $Inventory.Receipt 'InstallLayout') -eq 'usbip-slim-v1' -and
      (Get-UsbipValue $Inventory.Receipt 'SourceSHA256') -eq $script:UsbipSourceHash -and
      (Test-UsbipSameLocation (Get-UsbipValue $Inventory.Receipt 'InstallLocation') $Inventory.Root) -and
      $Inventory.Entries.Count -eq 0) { $owner = 'managed'; $reason = 'slim-receipt' }
  # Exactly one upstream registration: two records (one per registry view) leave the origin ambiguous, and the
  # takeover would run each uninstaller in turn.
  elseif ($Inventory.Entries.Count -eq 1 -and $null -eq $Inventory.Receipt -and $Inventory.LegacyPayload -and
      @($Inventory.Entries | Where-Object { -not (Test-UsbipSameLocation (Get-UsbipValue $_ 'InstallLocation') $Inventory.Root) }).Count -eq 0) {
    $owner = 'legacy-owned'; $reason = 'same-version-no-extras-bundled-installer'
  }
  [pscustomobject]@{ ownership = $owner; compatible = $compatible; reason = $reason }
}

function Get-UsbipOwnership([string]$Root, [string]$HostRoot) {
  try { Resolve-UsbipOwnership (Get-UsbipInventory $Root $HostRoot) }
  catch { [pscustomobject]@{ ownership = 'external'; compatible = $false; reason = 'inspection-failed' } }
}
Export-ModuleMember -Function *-Usbip*
