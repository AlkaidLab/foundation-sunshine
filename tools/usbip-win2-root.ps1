#requires -version 5.1
<##
  Owns the usbip-win2 root-enumerated device used by the DualSense path.

  The signed usbip-win2 driver is deliberately not installed or replaced by
  this helper.  It only registers the root device that the existing INF
  matches, removes duplicate instances of that exact hardware ID, and checks
  the driver's own `usbip port` health command.

  Microsoft references:
    Source records: ms-setupapi-createdeviceinfow-20261107 (§SetupDiCreateDeviceInfoW),
    ms-setupapi-setdeviceregistrypropertyw-20261107 (§SetupDiSetDeviceRegistryPropertyW),
    ms-setupapi-callclassinstaller-20261107 (§DIF_REGISTERDEVICE/DIF_REMOVE),
    ms-newdev-diinstalldevice-20261107 (§DiInstallDevice),
    ms-setupapi-getdeviceregistrypropertyw-20261107 (§SPDRP_HARDWAREID),
    ms-setupapi-getclassdevsw-20261107 (§SetupDiGetClassDevsW),
    ms-setupapi-enumdeviceinfo-20261107 (§SetupDiEnumDeviceInfo).
    SetupDiCreateDeviceInfoW / DICD_GENERATE_ID
    https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdicreatedeviceinfow
    SetupDiSetDeviceRegistryPropertyW
    https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdisetdeviceregistrypropertyw
    SetupDiCallClassInstaller / DIF_REGISTERDEVICE and DIF_REMOVE
    https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdicallclassinstaller
    DiInstallDevice
    https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diinstalldevice
##>

[CmdletBinding()]
param(
  [ValidateSet('install', 'repair', 'status', 'uninstall')]
  [string]$Action = 'status'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$HardwareId = 'ROOT\USBIP_WIN2\UDE'
$RootName = 'ALKAIDUSB'
$OldInstanceId = 'ROOT\USB\0000'
$UsbClassGuid = [Guid]'36FC9E60-C465-11CF-8056-444553540000'
$DICD_GENERATE_ID = 0x1
$DIF_PROPERTYCHANGE = 0x12
$DIF_REGISTERDEVICE = 0x19
$DIF_REMOVE = 0x5
$DICS_ENABLE = 0x1
$DICS_DISABLE = 0x2
$DICS_FLAG_GLOBAL = 0x1
$SPDRP_DEVICEDESC = 0x0
$SPDRP_HARDWAREID = 0x1
$SPDRP_FRIENDLYNAME = 0xC
$DIGCF_PRESENT = 0x2
$DIGCF_ALLCLASSES = 0x4
$ERROR_NO_MORE_ITEMS = 259

if (-not ('Alkaid.SetupApi' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Alkaid {
  public static class SetupApi {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DEVINFO_DATA {
      public uint cbSize;
      public Guid ClassGuid;
      public uint DevInst;
      public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_CLASSINSTALL_HEADER {
      public uint cbSize;
      public uint InstallFunction;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_PROPCHANGE_PARAMS {
      public SP_CLASSINSTALL_HEADER ClassInstallHeader;
      public uint StateChange;
      public uint Scope;
      public uint HwProfile;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(
      ref Guid ClassGuid, string Enumerator, IntPtr hwndParent, uint Flags);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(
      IntPtr ClassGuid, string Enumerator, IntPtr hwndParent, uint Flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern IntPtr SetupDiCreateDeviceInfoList(
      ref Guid ClassGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiCreateDeviceInfo(
      IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid,
      string DeviceDescription, IntPtr hwndParent, uint CreationFlags,
      ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiEnumDeviceInfo(
      IntPtr DeviceInfoSet, uint MemberIndex, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiGetDeviceRegistryProperty(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, uint Property,
      out uint PropertyRegDataType, byte[] PropertyBuffer, uint PropertyBufferSize,
      out uint RequiredSize);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiGetDeviceInstanceId(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
      StringBuilder DeviceInstanceId, uint DeviceInstanceIdSize, out uint RequiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiSetDeviceRegistryProperty(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, uint Property,
      byte[] PropertyBuffer, uint PropertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiSetClassInstallParams(
      IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
      IntPtr ClassInstallParams, uint ClassInstallParamsSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiCallClassInstaller(
      uint InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("newdev.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DiInstallDevice(
      IntPtr hwndParent, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
      uint Flags, [MarshalAs(UnmanagedType.Bool)] out bool NeedReboot);
  }
}
'@
}

function Get-Win32Error {
  return [Runtime.InteropServices.Marshal]::GetLastWin32Error()
}

function Throw-Win32([string]$Operation) {
  throw "$Operation failed with Win32 error $(Get-Win32Error)."
}

function Get-PropertyString([IntPtr]$Set, $Data, [uint32]$Property) {
  [uint32]$regType = 0
  [uint32]$required = 0
  $empty = New-Object byte[] 0
  [Alkaid.SetupApi]::SetupDiGetDeviceRegistryProperty(
    $Set, [ref]$Data, $Property, [ref]$regType, $empty, 0, [ref]$required) | Out-Null
  if ($required -eq 0) { return '' }
  $buffer = New-Object byte[] $required
  if (-not [Alkaid.SetupApi]::SetupDiGetDeviceRegistryProperty(
      $Set, [ref]$Data, $Property, [ref]$regType, $buffer, $buffer.Length, [ref]$required)) {
    return ''
  }
  $unicode_layout = $buffer.Length -ge 2 -and ($buffer.Length % 2 -eq 0)
  if ($unicode_layout) {
    for ($offset = 1; $offset -lt $buffer.Length; $offset += 2) {
      if ($buffer[$offset] -ne 0) {
        $unicode_layout = $false
        break
      }
    }
  }
  $encoding = if ($unicode_layout) { [Text.Encoding]::Unicode } else { [Text.Encoding]::Default }
  return $encoding.GetString($buffer).Trim([char]0)
}

function Get-InstanceId([IntPtr]$Set, $Data) {
  [uint32]$required = 0
  $probe = New-Object Text.StringBuilder 1
  [Alkaid.SetupApi]::SetupDiGetDeviceInstanceId(
    $Set, [ref]$Data, $probe, 1, [ref]$required) | Out-Null
  if ($required -eq 0) { return '' }
  $id = New-Object Text.StringBuilder ([int]$required)
  if (-not [Alkaid.SetupApi]::SetupDiGetDeviceInstanceId(
      $Set, [ref]$Data, $id, $id.Capacity, [ref]$required)) {
    return ''
  }
  return $id.ToString()
}

function Get-DeviceRecords([bool]$AllClasses) {
  if ($AllClasses) {
    $set = [Alkaid.SetupApi]::SetupDiGetClassDevs(
      [IntPtr]::Zero, [NullString]::Value, [IntPtr]::Zero, $DIGCF_PRESENT -bor $DIGCF_ALLCLASSES)
  }
  else {
    $class = $UsbClassGuid
    $set = [Alkaid.SetupApi]::SetupDiGetClassDevs(
      [ref]$class, [NullString]::Value, [IntPtr]::Zero, $DIGCF_PRESENT)
  }
  if ($set.ToInt64() -eq -1) { Throw-Win32 'SetupDiGetClassDevs' }

  $records = New-Object System.Collections.Generic.List[object]
  try {
    for ($index = 0; ; $index++) {
      $data = New-Object Alkaid.SetupApi+SP_DEVINFO_DATA
      $data.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($data)
      $enum_ok = [Alkaid.SetupApi]::SetupDiEnumDeviceInfo($set, $index, [ref]$data)
      $enum_error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      if (-not $enum_ok) {
        if ($enum_error -eq $ERROR_NO_MORE_ITEMS) { break }
        Throw-Win32 'SetupDiEnumDeviceInfo'
      }
      $hardware = Get-PropertyString $set $data $SPDRP_HARDWAREID
      $records.Add([pscustomobject]@{
        Set = $set
        Data = $data
        InstanceId = Get-InstanceId $set $data
        HardwareId = $hardware
        Description = Get-PropertyString $set $data $SPDRP_DEVICEDESC
        FriendlyName = Get-PropertyString $set $data $SPDRP_FRIENDLYNAME
      })
    }
    return $records.ToArray()
  }
  catch {
    [Alkaid.SetupApi]::SetupDiDestroyDeviceInfoList($set) | Out-Null
    throw
  }
}

function Close-DeviceRecords($Records) {
  $seen = @{}
  foreach ($record in @($Records)) {
    $set = $record.Set
    if (-not $set) { continue }
    $key = $set.ToInt64()
    if ($key -eq 0 -or $key -eq -1 -or $seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    [Alkaid.SetupApi]::SetupDiDestroyDeviceInfoList($set) | Out-Null
  }
}

function Test-HardwareId($Record) {
  $matches = @($Record.HardwareId -split ([char]0) | Where-Object {
      $_.Trim() -ieq $HardwareId
    })
  return $matches.Length -gt 0
}

function Test-CanonicalRecord($Record) {
  return $Record.FriendlyName -ieq $RootName -or
    $Record.Description -ieq $RootName -or
    $Record.InstanceId -imatch ('\\' + [regex]::Escape($RootName) + '($|\\)')
}

function Set-MultiStringProperty([IntPtr]$Set, $Data, [uint32]$Property, [string[]]$Values) {
  $payload = (($Values -join [char]0) + [char]0 + [char]0)
  $bytes = [Text.Encoding]::Unicode.GetBytes($payload)
  if (-not [Alkaid.SetupApi]::SetupDiSetDeviceRegistryProperty(
      $Set, [ref]$Data, $Property, $bytes, $bytes.Length)) {
    Throw-Win32 'SetupDiSetDeviceRegistryProperty'
  }
}

function Set-StringProperty([IntPtr]$Set, $Data, [uint32]$Property, [string]$Value) {
  $bytes = [Text.Encoding]::Unicode.GetBytes($Value + [char]0)
  if (-not [Alkaid.SetupApi]::SetupDiSetDeviceRegistryProperty(
      $Set, [ref]$Data, $Property, $bytes, $bytes.Length)) {
    Throw-Win32 'SetupDiSetDeviceRegistryProperty'
  }
}

function Set-DeviceState([IntPtr]$Set, $Data, [uint32]$State) {
  $params = New-Object Alkaid.SetupApi+SP_PROPCHANGE_PARAMS
  $params.ClassInstallHeader.cbSize = [Runtime.InteropServices.Marshal]::SizeOf(
    [type]'Alkaid.SetupApi+SP_CLASSINSTALL_HEADER')
  $params.ClassInstallHeader.InstallFunction = $DIF_PROPERTYCHANGE
  $params.StateChange = $State
  $params.Scope = $DICS_FLAG_GLOBAL
  $params.HwProfile = 0
  $size = [Runtime.InteropServices.Marshal]::SizeOf($params)
  $memory = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
  try {
    [Runtime.InteropServices.Marshal]::StructureToPtr($params, $memory, $false)
    if (-not [Alkaid.SetupApi]::SetupDiSetClassInstallParams($Set, [ref]$Data, $memory, $size) -or
        -not [Alkaid.SetupApi]::SetupDiCallClassInstaller($DIF_PROPERTYCHANGE, $Set, [ref]$Data)) {
      Throw-Win32 'SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)'
    }
  }
  finally {
    [Runtime.InteropServices.Marshal]::FreeHGlobal($memory)
  }
}

function Remove-DeviceRecord($Record) {
  $data = $Record.Data
  if ([Alkaid.SetupApi]::SetupDiCallClassInstaller($DIF_REMOVE, $Record.Set, [ref]$data)) {
    return $true
  }
  try {
    Set-DeviceState $Record.Set $Record.Data $DICS_DISABLE
    return $false
  }
  catch {
    throw "Unable to remove or disable device '$($Record.InstanceId)': $($_.Exception.Message)"
  }
}

function Disable-LegacyDevice {
  $records = @(Get-DeviceRecords $true)
  try {
    foreach ($record in $records) {
      if ($record.InstanceId -ieq $OldInstanceId) {
        try {
          Set-DeviceState $record.Set $record.Data $DICS_DISABLE
          Write-Verbose "Disabled legacy device $OldInstanceId."
        }
        catch {
          Write-Warning "Could not disable legacy device ${OldInstanceId}: $($_.Exception.Message)"
        }
      }
    }
  }
  finally { Close-DeviceRecords $records }
}

function New-RootDevice {
  $set = [Alkaid.SetupApi]::SetupDiCreateDeviceInfoList([ref]$UsbClassGuid, [IntPtr]::Zero)
  if ($set -eq [IntPtr]::new(-1)) { Throw-Win32 'SetupDiCreateDeviceInfoList' }
  $data = New-Object Alkaid.SetupApi+SP_DEVINFO_DATA
  $data.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($data)
  try {
    if (-not [Alkaid.SetupApi]::SetupDiCreateDeviceInfo(
        $set, $RootName, [ref]$UsbClassGuid, $RootName, [IntPtr]::Zero,
        $DICD_GENERATE_ID, [ref]$data)) {
      Throw-Win32 'SetupDiCreateDeviceInfoW'
    }
    Set-MultiStringProperty $set $data $SPDRP_HARDWAREID @($HardwareId)
    Set-StringProperty $set $data $SPDRP_FRIENDLYNAME $RootName
    if (-not [Alkaid.SetupApi]::SetupDiCallClassInstaller($DIF_REGISTERDEVICE, $set, [ref]$data)) {
      Throw-Win32 'SetupDiCallClassInstaller(DIF_REGISTERDEVICE)'
    }
    [bool]$needReboot = $false
    if (-not [Alkaid.SetupApi]::DiInstallDevice(
        [IntPtr]::Zero, $set, [ref]$data, 0, [ref]$needReboot)) {
      Throw-Win32 'DiInstallDevice'
    }
    return [pscustomobject]@{ Set = $set; Data = $data; NeedReboot = $needReboot }
  }
  catch {
    [Alkaid.SetupApi]::SetupDiCallClassInstaller($DIF_REMOVE, $set, [ref]$data) | Out-Null
    [Alkaid.SetupApi]::SetupDiDestroyDeviceInfoList($set) | Out-Null
    throw
  }
}

function Ensure-RootDevice {
  $records = @(Get-DeviceRecords $true | Where-Object { Test-HardwareId $_ })
  $created = $null
  try {
    $canonical = @($records | Where-Object { Test-CanonicalRecord $_ }) | Select-Object -First 1
    if (-not $canonical -and $records.Count -eq 1) {
      # Older usbip-win2 releases did not expose the friendly name. Reuse the
      # sole exact-HWID node rather than creating a second active controller.
      $canonical = $records[0]
    }
    if (-not $canonical) {
      $created = New-RootDevice
      $canonical = [pscustomobject]@{
        Set = $created.Set
        Data = $created.Data
        InstanceId = Get-InstanceId $created.Set $created.Data
        HardwareId = $HardwareId
        Description = $RootName
        FriendlyName = $RootName
      }
      Write-Output "Created root device $RootName ($($canonical.InstanceId))."
    }
    else {
      Write-Output "Reusing root device $($canonical.InstanceId)."
      if ($canonical.FriendlyName -ine $RootName) {
        try { Set-StringProperty $canonical.Set $canonical.Data $SPDRP_FRIENDLYNAME $RootName } catch { Write-Verbose $_ }
      }
    }
    foreach ($duplicate in @($records | Where-Object { $_.InstanceId -ne $canonical.InstanceId })) {
      $removed = Remove-DeviceRecord $duplicate
      Write-Output "$(if ($removed) { 'Removed' } else { 'Disabled' }) duplicate usbip root $($duplicate.InstanceId)."
    }
    Disable-LegacyDevice
    return [pscustomobject]@{ RootPresent = $true; InstanceId = $canonical.InstanceId; NeedReboot = [bool]($created -and $created.NeedReboot) }
  }
  catch {
    if ($created) {
      try { [Alkaid.SetupApi]::SetupDiCallClassInstaller($DIF_REMOVE, $created.Set, [ref]$created.Data) | Out-Null } catch { Write-Verbose $_ }
    }
    throw
  }
  finally {
    Close-DeviceRecords $records
    if ($created) { [Alkaid.SetupApi]::SetupDiDestroyDeviceInfoList($created.Set) | Out-Null }
  }
}

function Remove-RootDevices {
  $records = @(Get-DeviceRecords $true | Where-Object { Test-HardwareId $_ })
  try {
    foreach ($record in $records) {
      $removed = Remove-DeviceRecord $record
      Write-Output "$(if ($removed) { 'Removed' } else { 'Disabled' }) usbip root $($record.InstanceId)."
    }
  }
  finally { Close-DeviceRecords $records }
}

function Find-UsbipExecutable {
  $candidates = @(
    (Join-Path ${env:ProgramFiles} 'USBip\usbip.exe'),
    (Join-Path ${env:ProgramFiles} 'usbip-win2\usbip.exe'),
    (Join-Path ${env:ProgramFiles} 'USBIP-WIN2\usbip.exe'),
    (Join-Path $PSScriptRoot 'usbip.exe')
  )
  try { $candidates += (Get-Command usbip.exe -ErrorAction Stop).Source } catch { }
  return @($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1)
}

function Get-VhciStatus {
  $executable = Find-UsbipExecutable
  if (-not $executable) {
    return [pscustomobject]@{ Healthy = $false; Executable = ''; ExitCode = $null; Output = 'usbip.exe not found'; MissingVhci = $true }
  }
  $info = New-Object Diagnostics.ProcessStartInfo
  $info.FileName = $executable
  $info.Arguments = 'port'
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $process = New-Object Diagnostics.Process
  $process.StartInfo = $info
  try {
    $process.Start() | Out-Null
    if (-not $process.WaitForExit(10000)) {
      $process.Kill()
      $process.WaitForExit()
      return [pscustomobject]@{ Healthy = $false; Executable = $executable; ExitCode = $null; Output = 'usbip port timed out'; MissingVhci = $false }
    }
    $output = ($process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()).Trim()
    $missing = $output -match '(?i)(vhci[^\r\n]*not found|not found[^\r\n]*vhci|unable to find[^\r\n]*vhci)'
    return [pscustomobject]@{
      Healthy = ($process.ExitCode -eq 0 -and -not $missing)
      Executable = $executable
      ExitCode = $process.ExitCode
      Output = $output
      MissingVhci = $missing
    }
  }
  finally { $process.Dispose() }
}

function Write-Result([string]$Operation, [bool]$RootPresent, [string]$InstanceId, [bool]$NeedReboot, $Port) {
  $fallback = if ($Port.Healthy) { '' } else { 'DS4 fallback remains active because the usbip-win2 VHCI is unavailable.' }
  [ordered]@{
    operation = $Operation
    root_device = $RootName
    hardware_id = $HardwareId
    instance_id = $InstanceId
    root_present = $RootPresent
    usbip_healthy = [bool]$Port.Healthy
    usbip_executable = $Port.Executable
    usbip_exit_code = $Port.ExitCode
    usbip_output = $Port.Output
    vhci_missing = [bool]$Port.MissingVhci
    ds4_fallback = $fallback
    reboot_required = $NeedReboot
  } | ConvertTo-Json -Compress
}

try {
  if ($Action -ne 'status' -and -not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrative privileges are required for this action.'
  }

  switch ($Action) {
    'status' {
      $records = @(Get-DeviceRecords $true | Where-Object { Test-HardwareId $_ })
      try {
        $canonical = @($records | Where-Object { Test-CanonicalRecord $_ }) | Select-Object -First 1
        $port = Get-VhciStatus
        Write-Result 'status' ($null -ne $canonical) $(if ($canonical) { $canonical.InstanceId } else { '' }) $false $port
        if (-not $port.Healthy -or -not $canonical) { exit 2 }
      }
      finally { Close-DeviceRecords $records }
    }
    'install' {
      $root = Ensure-RootDevice
      $port = Get-VhciStatus
      Write-Result 'install' $root.RootPresent $root.InstanceId $root.NeedReboot $port
    }
    'repair' {
      $root = Ensure-RootDevice
      $port = Get-VhciStatus
      Write-Result 'repair' $root.RootPresent $root.InstanceId $root.NeedReboot $port
      if (-not $port.Healthy) { exit 2 }
    }
    'uninstall' {
      Remove-RootDevices
      $port = Get-VhciStatus
      Write-Result 'uninstall' $false '' $false $port
    }
  }
  exit 0
}
catch {
  [ordered]@{
    operation = $Action
    root_device = $RootName
    hardware_id = $HardwareId
    success = $false
    error = $_.Exception.Message
    ds4_fallback = 'DS4 fallback remains active because the usbip-win2 root device could not be prepared.'
  } | ConvertTo-Json -Compress
  exit 1
}
