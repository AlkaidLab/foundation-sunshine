#requires -version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:RecordKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AlkaidLabUSBip'
$script:HardwareId = 'ROOT\USBIP_WIN2\UDE'
$script:DetachTask = 'USBip Detach All On Reboot Or Shutdown'
$script:NeedsRestart = $false
Import-Module (Join-Path $PSScriptRoot 'usbip-package-ownership.psm1') -Force -DisableNameChecking

function Invoke-UsbipCommand([string]$File, [string[]]$Arguments) {
  & $File @Arguments | Out-Host
  $rc = $LASTEXITCODE
  if ($rc -eq 3010) { $script:NeedsRestart = $true }
  elseif ($rc -ne 0) { throw "$([IO.Path]::GetFileName($File)) failed: $rc" }
}

function Read-UsbipManifest([string]$Root) {
  $manifest = Get-Content -LiteralPath (Join-Path $Root 'payload.json') -Raw | ConvertFrom-Json
  if ($manifest.schema -ne 1 -or $manifest.layout -ne 'usbip-slim-v1' -or $manifest.version -ne '0.9.8.1') { throw 'Unsupported USB/IP payload' }
  $required = @('usbip.exe', 'devnode.exe', 'libusbip.dll', 'resources.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'driver/usbip2_ude.inf', 'driver/usbip2_ude.sys', 'driver/usbip2_ude.cat',
    'driver/usbip2_filter.inf', 'driver/usbip2_filter.sys', 'driver/usbip2_filter.cat')
  $names = @($manifest.files.PSObject.Properties.Name)
  if (@(Compare-Object $required $names).Count -ne 0) { throw 'Incomplete or unexpected USB/IP file list' }
  foreach ($file in $manifest.files.PSObject.Properties) {
    $path = Join-Path $Root $file.Name
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.Value.sha256) { throw "USB/IP file hash mismatch: $($file.Name)" }
  }
  if (-not (Test-Path -LiteralPath (Join-Path $Root 'LICENSE'))) { throw 'USB/IP license is missing' }
  return $manifest
}

function Get-UsbipLegacyEntries {
  # Fixed upstream AppId from usbip-slim-setup-20261117, GetInstalledUninstallString.
  foreach ($base in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
    $key = Join-Path $base '{199505b0-b93d-4521-a8c7-897818e0205a}_is1'
    if (Test-Path -LiteralPath $key) { Get-ItemProperty -LiteralPath $key }
  }
}

function Remove-UsbipLegacy([string]$Root, [string]$HostRoot, [switch]$ConfirmedReplace) {
  $owner = Get-UsbipOwnership $Root $HostRoot -ConfirmedReplace:$ConfirmedReplace
  if ($owner.ownership -ne 'legacy-owned') { return }
  foreach ($entry in @(Get-UsbipLegacyEntries)) {
    # Inno Setup registers the uninstaller as: "<dir>\unins000.exe" /LOG  (observed with usbip-win2 0.9.8.1).
    # Accept only that exact optional argument; the arguments actually run below are fixed.
    if ($entry.UninstallString -notmatch '^"([^"]+\\unins[0-9]+\.exe)"(?:\s+/LOG)?$') { throw 'Unrecognized upstream USB/IP uninstaller; remove it manually' }
    $uninstaller = $Matches[1]
    $process = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    if ($process.ExitCode -notin 0, 3010) { throw "Upstream USB/IP uninstall failed: $($process.ExitCode)" }
    $script:NeedsRestart = $true
  }
  if (@(Get-UsbipLegacyEntries).Count -ne 0) { throw 'Upstream USB/IP registration remains; replacement was not attempted' }
}

# usbip-replace-ms-classdevs-20261121: DIGCF_ALLCLASSES without DIGCF_PRESENT
# includes phantom nodes. usbip-replace-ms-diuninstall-20261121: remove the
# exact device element and preserve the NeedReboot result.
# usbip-replace-ms-openinfo-20261121: resolve the exact instance in the set.
# usbip-replace-ms-addtype-20261121: compile native interop in Windows PowerShell.
function Remove-UsbipAbsentNode([string]$InstanceId) {
  if (-not ('UsbipReplace.DeviceRemoval' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace UsbipReplace {
  public static class DeviceRemoval {
    [StructLayout(LayoutKind.Sequential)]
    public struct DeviceInfo { public uint Size; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(IntPtr cls, string enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string id, IntPtr hwnd, uint flags, ref DeviceInfo data);
    [DllImport("newdev.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool DiUninstallDevice(IntPtr hwnd, IntPtr set, ref DeviceInfo data, uint flags, [MarshalAs(UnmanagedType.Bool)] out bool reboot);
    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    public static bool Remove(string id) {
      IntPtr set = SetupDiGetClassDevsW(IntPtr.Zero, null, IntPtr.Zero, 4);
      if (set == new IntPtr(-1)) throw new ExternalException("USB/IP device operation failed", Marshal.GetLastWin32Error());
      try {
        DeviceInfo data = new DeviceInfo(); data.Size = (uint)Marshal.SizeOf(typeof(DeviceInfo));
        if (!SetupDiOpenDeviceInfoW(set, id, IntPtr.Zero, 0, ref data)) throw new ExternalException("USB/IP device operation failed", Marshal.GetLastWin32Error());
        bool reboot;
        if (!DiUninstallDevice(IntPtr.Zero, set, ref data, 0, out reboot)) throw new ExternalException("USB/IP device operation failed", Marshal.GetLastWin32Error());
        return reboot;
      } finally { SetupDiDestroyDeviceInfoList(set); }
    }
  }
}
'@
  }
  return [UsbipReplace.DeviceRemoval]::Remove($InstanceId)
}

function Remove-UsbipAbsentLegacyNodes([string]$HostRoot) {
  $present = @{}
  foreach ($node in @(Get-UsbipNodes)) { $present[$node.InstanceId] = $true }
  foreach ($node in @(Get-UsbipNodes -IncludeAbsent)) {
    $id = [string]$node.InstanceId
    if ($present.ContainsKey($id)) { continue }
    # Recheck immediately before removal; never intentionally remove a live controller.
    if (@(Get-UsbipNodes | Where-Object { $_.InstanceId -eq $id }).Count -ne 0) { continue }
    $reboot = Remove-UsbipAbsentNode $id
    if ($reboot) { $script:NeedsRestart = $true }
    $line = "USB/IP removed absent controller $id; reboot required: $reboot"
    Write-Host $line
    try {
      Add-Content -LiteralPath (Join-Path $HostRoot 'scripts\usbip-replace-nodes.log') -Value $line -Encoding UTF8
    } catch {
      Write-Warning "USB/IP removal log write failed: $($_.Exception.Message)"
    }
  }
}

function Get-UsbipOemPackages {
  # usbip-slim-dism-20261117, -Online / -All: structured output, no localized
  # pnputil text parsing and no writes to the DriverDatabase registry.
  Get-WindowsDriver -Online -All -ErrorAction Stop | Where-Object {
    [IO.Path]::GetFileName($_.OriginalFileName) -in 'usbip2_ude.inf', 'usbip2_filter.inf' -and $_.Driver -match '^oem[0-9]+\.inf$'
  }
}

function Set-UsbipDetachTask([string]$Root, [switch]$Remove) {
  # usbip-slim-setup-20261117, RegisterDetachTask; Task Scheduler 2.0.
  $scheduler = New-Object -ComObject 'Schedule.Service'
  $scheduler.Connect()
  $folder = $scheduler.GetFolder('\')
  if ($Remove) {
    if (@($folder.GetTasks(0) | Where-Object { $_.Name -eq $script:DetachTask }).Count -eq 0) { return }
    $folder.DeleteTask($script:DetachTask, 0)
    return
  }
  $task = $scheduler.NewTask(0)
  $task.RegistrationInfo.Description = 'Detach imported USB devices on reboot or shutdown'
  $trigger = $task.Triggers.Create(0)
  $trigger.Subscription = '<QueryList><Query Id="0" Path="System"><Select Path="System">*[System[Provider[@Name=''Microsoft-Windows-Kernel-Power''] and (EventID=109)]] or *[System[Provider[@Name=''User32''] and (EventID=1074)]]</Select></Query></QueryList>'
  $task.Principal.UserId = 'S-1-5-19'
  $task.Principal.LogonType = 5
  $task.Principal.RunLevel = 0
  $task.Settings.Enabled = $true
  $task.Settings.MultipleInstances = 2
  $task.Settings.DisallowStartIfOnBatteries = $false
  $task.Settings.StopIfGoingOnBatteries = $false
  $task.Settings.AllowHardTerminate = $true
  $task.Settings.StartWhenAvailable = $false
  $task.Settings.RunOnlyIfNetworkAvailable = $false
  $task.Settings.ExecutionTimeLimit = 'PT30S'
  $task.Settings.Priority = 7
  $action = $task.Actions.Create(0)
  $action.Path = Join-Path $Root 'usbip.exe'
  $action.Arguments = 'detach --all=closeonly'
  $null = $folder.RegisterTaskDefinition($script:DetachTask, $task, 6, 'S-1-5-19', $null, 5)
}

function Remove-UsbipPackage([string]$Root, [switch]$KeepFiles) {
  if ((Get-UsbipOwnership $Root (Split-Path -Parent $PSScriptRoot)).ownership -ne 'managed') { return }
  $failures = @()
  # Each cleanup stage runs even if an earlier one failed; preserve recovery
  # tools and the receipt until all OS removal steps have succeeded.
  try { Set-UsbipDetachTask $Root -Remove } catch { $failures += $_ }
  try {
    if ((Test-Path -LiteralPath (Join-Path $Root 'devnode.exe')) -and @(Get-UsbipNodes -IncludeAbsent).Count -gt 0) {
      Invoke-UsbipCommand (Join-Path $Root 'devnode.exe') @('remove', $script:HardwareId, 'root')
    }
  } catch { $failures += $_ }
  try {
    foreach ($package in @(Get-UsbipOemPackages)) {
      try { Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/delete-driver', $package.Driver, '/uninstall', '/force') }
      catch { $failures += $_ }
    }
  } catch { $failures += $_ }
  if ($failures.Count -ne 0) { throw "USB/IP cleanup incomplete ($($failures.Count) failed steps); recovery files retained" }
  if (Test-Path -LiteralPath $script:RecordKey) { Remove-Item -LiteralPath $script:RecordKey -Recurse -Force }
  if (-not $KeepFiles) { Remove-UsbipFiles $Root }
}

function Remove-UsbipFiles([string]$Root) {
  # Never remove unrelated files or a legacy installation recursively.
  if (Test-Path -LiteralPath (Join-Path $Root 'payload.json')) {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $Root 'payload.json') | ConvertFrom-Json
    foreach ($name in @($manifest.files.PSObject.Properties.Name) + @('payload.json', 'LICENSE', 'usbip-package.ps1', 'usbip-package-functions.psm1', 'usbip-package-ownership.psm1', 'usbip-roothub-guard.ps1')) {
      if ($name -notmatch '^(driver/)?[A-Za-z0-9_.-]+$') { throw 'Invalid receipt member' }
      $path = Join-Path $Root $name
      if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    $driver = Join-Path $Root 'driver'
    if ((Test-Path $driver) -and @(Get-ChildItem -Force $driver).Count -eq 0) { Remove-Item -LiteralPath $driver }
    if (@(Get-ChildItem -Force $Root).Count -eq 0) { Remove-Item -LiteralPath $Root }
  }
}

function Get-UsbipNodes([switch]$IncludeAbsent) {
  # usbip-slim-ms-pnp-property-20261117: hardware IDs are device properties,
  # not instance IDs. Upstream creates ROOT\USB instances; later repairs may
  # use a different enumerator while retaining the exact hardware ID.
  # Get-PnpDevice -PresentOnly:$false is not "all devices": on Windows 11 it returned only the 8 non-present
  # ones, so the node of a live install was never found and uninstall skipped `devnode remove`, leaving a ghost
  # root node behind. Omit the switch to include absent nodes.
  $devices = if ($IncludeAbsent) { Get-PnpDevice -ErrorAction Stop } else { Get-PnpDevice -PresentOnly -ErrorAction Stop }
  foreach ($device in @($devices | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
    $ids = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction Stop).Data
    if ($ids -contains $script:HardwareId) { $device }
  }
}

# usbip-slim-ms-pnp-property-20261117: typed HardwareIds and ProblemCode;
# usbip-slim-ms-pnp-device-20261117: PresentOnly, independent of root enumerator name.
function Test-UsbipReady([string]$Root, $Manifest, [switch]$External, [switch]$AllowPendingNodes) {
  try {
    if (-not $External) { $null = Read-UsbipManifest $Root }
    $nodes = @(Get-UsbipNodes)
    if ($nodes.Count -ne 1 -and (-not $AllowPendingNodes -or $nodes.Count -lt 2)) { return $false }
    # usbip-replace-ms-prob-removed-20261121: 21 means Windows is removing the device.
    $usable = 0
    foreach ($node in $nodes) {
      $problem = (Get-PnpDeviceProperty -InstanceId $node.InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction Stop).Data
      if ($problem -in 0, 14) { $usable++ }
      elseif (-not $AllowPendingNodes -or $problem -ne 21) { return $false }
    }
    if ($usable -ne 1) { return $false }
    foreach ($name in 'usbip2_ude', 'usbip2_filter') {
      $service = Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Services\' + $name) -ErrorAction Stop
      $image = [string]$service.ImagePath
      $image = $image -replace '^\\SystemRoot\\', ($env:SystemRoot + '\') -replace '^\\\?\?\\', ''
      if (-not [IO.Path]::IsPathRooted($image)) { $image = Join-Path $env:SystemRoot $image }
      $expected = $Manifest.files.PSObject.Properties['driver/' + $name + '.sys'].Value.sha256
      if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash -ne $expected) { return $false }
    }
    return $true
  } catch { return $false }
}

function Test-UsbipExternalReady([string]$Root) {
  try {
    if ((Get-FileHash -LiteralPath (Join-Path $Root 'usbip.exe') -Algorithm SHA256).Hash -ne '817da219fa153b4b37c3bc1aa8fcc63f33e046188c57cc3dd2c60a6ef0883c56') { return $false }
    if ((Get-FileHash -LiteralPath (Join-Path $Root 'libusbip.dll') -Algorithm SHA256).Hash -ne 'f123ef5d288b817a890cbe39a3c2d212b3d8b1765c691c8703ccb76c7337986c') { return $false }
    if ((Get-FileHash -LiteralPath (Join-Path $Root 'resources.dll') -Algorithm SHA256).Hash -ne '4bfc862e5891d9b4c12a8ebd43d3ba871ebf94b65f7a00c22527c905949e0fff') { return $false }
    $manifest = [pscustomobject]@{ files = [pscustomobject]@{
      'driver/usbip2_ude.sys' = [pscustomobject]@{ sha256 = 'abd4fc43dce40e027e4acc884cbfe92433e026206f6565c5ee5284a39441169a' }
      'driver/usbip2_filter.sys' = [pscustomobject]@{ sha256 = 'b93a5fc3c23d56f9b49634742374e32f269e53f764caa2d5fcb43e3259f7eced' }
    } }
    return Test-UsbipReady $Root $manifest -External
  } catch { return $false }
}

function Install-UsbipPackage([string]$Payload, [string]$Root, [string]$Scripts, [string]$HostRoot, [switch]$ConfirmedReplace) {
  $owner = Get-UsbipOwnership $Root $HostRoot -ConfirmedReplace:$ConfirmedReplace
  if ($owner.ownership -eq 'external') { throw 'Externally managed USB/IP must not be replaced' }
  $manifest = Read-UsbipManifest $Payload
  Remove-UsbipLegacy $Root $HostRoot -ConfirmedReplace:$ConfirmedReplace
  if ($owner.ownership -eq 'legacy-owned') { Remove-UsbipAbsentLegacyNodes $HostRoot }
  # A partial prior slim install must be removed before replacing its tools.
  if ((Test-Path -LiteralPath $script:RecordKey) -or (Test-Path -LiteralPath (Join-Path $Root 'payload.json'))) { Remove-UsbipPackage $Root }
  New-Item -ItemType Directory -Path $Root -Force | Out-Null
  foreach ($name in @($manifest.files.PSObject.Properties.Name) + @('payload.json', 'LICENSE')) {
    $destination = Join-Path $Root $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Payload $name) -Destination $destination -Force
  }
  foreach ($name in 'usbip-package.ps1', 'usbip-package-functions.psm1', 'usbip-package-ownership.psm1', 'usbip-roothub-guard.ps1') {
    Copy-Item -LiteralPath (Join-Path $Scripts $name) -Destination (Join-Path $Root $name) -Force
  }
  $null = Read-UsbipManifest $Root
  # Record ownership before the first driver mutation so failure cleanup is owned.
  New-Item -Path $script:RecordKey -Force | Out-Null
  foreach ($entry in @{
    DisplayName = "USBip $($manifest.version)"; DisplayVersion = $manifest.version; Publisher = 'usbip-win2';
    InstallLocation = $Root; InstallLayout = $manifest.layout; SourceSHA256 = $manifest.installer_sha256;
    UninstallString = ('"{0}\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "{1}\usbip-package.ps1" -Action uninstall' -f $env:SystemRoot, $Root)
  }.GetEnumerator()) {
    New-ItemProperty -LiteralPath $script:RecordKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
  }
  New-ItemProperty -LiteralPath $script:RecordKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
  New-ItemProperty -LiteralPath $script:RecordKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
  # usbip-slim-pnputil-20261117 /add-driver. Never edit an INF or register
  # services manually: the signed INF owns AddService/AddReg/AddFilter.
  Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/add-driver', (Join-Path $Root 'driver\usbip2_ude.inf'))
  Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/add-driver', (Join-Path $Root 'driver\usbip2_filter.inf'), '/install')
  Invoke-UsbipCommand (Join-Path $Root 'devnode.exe') @('install', (Join-Path $Root 'driver\usbip2_ude.inf'), $script:HardwareId)
  Set-UsbipDetachTask $Root
  if (-not (Test-UsbipReady $Root $manifest)) {
    if (-not $script:NeedsRestart -or -not (Test-UsbipReady $Root $manifest -AllowPendingNodes)) { throw 'USB/IP controller or registered driver did not verify' }
    Write-Warning 'USB/IP readiness could not be verified before the required restart; check again after restart.'
  }
  # The bundled upstream installer was only the proof that an earlier FS install owned the package.
  # After a verified replacement it is a 26 MB leftover of the old layout.
  if ($owner.ownership -eq 'legacy-owned') {
    $bundled = Join-Path $HostRoot 'scripts\usbip\USBip-0.9.8.1-x64.exe'
    if (Test-Path -LiteralPath $bundled -PathType Leaf) { Remove-Item -LiteralPath $bundled -Force }
  }
}

function Get-UsbipRestartCode { if ($script:NeedsRestart) { return 3010 }; return 0 }
Export-ModuleMember -Function *-Usbip*
