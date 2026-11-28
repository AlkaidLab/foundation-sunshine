#requires -version 5.1
<##
  Root-device regression tests with an in-memory SetupAPI substitute.
  No device, driver, registry, ownership receipt or administrator access is used.
  Run with Windows PowerShell 5.1 or PowerShell 7.
##>
[CmdletBinding()]
param([string]$HelperScript = (Join-Path $PSScriptRoot '../../tools/usbip-win2-root.ps1'))

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $HelperScript).Path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
# Load only functions, bypassing the command entry point and its machine probes.
foreach ($definition in $ast.FindAll({
    param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
  }, $false)) {
  . ([scriptblock]::Create($definition.Extent.Text))
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Alkaid {
  public static class SetupApi {
    [StructLayout(LayoutKind.Sequential)]
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
    public static bool ParamsOk = true, InstallerOk = true;
    public static int ParamsCalls, InstallerCalls, ClosedSets;
    public static bool SetupDiSetClassInstallParams(IntPtr set,
        ref SP_DEVINFO_DATA data, IntPtr memory, uint size) {
      ++ParamsCalls;
      return ParamsOk;
    }
    public static bool SetupDiCallClassInstaller(uint operation, IntPtr set,
        ref SP_DEVINFO_DATA data) {
      ++InstallerCalls;
      return InstallerOk;
    }
    public static bool SetupDiDestroyDeviceInfoList(IntPtr set) {
      ++ClosedSets;
      return true;
    }
  }
}
'@

$HardwareId = 'ROOT\USBIP_WIN2\UDE'
$RootName = 'ALKAIDUSB'
$OldInstanceId = 'ROOT\USB\0000'
$SPDRP_FRIENDLYNAME = 0xC
$DIF_PROPERTYCHANGE = 0x12
$DIF_REMOVE = 0x5
$DICS_DISABLE = 0x2
$DICS_FLAG_GLOBAL = 0x1

function Get-DeviceRecords([bool]$AllClasses) { return $script:devices }
function Get-Win32Error { return 203 }
function New-RootDevice {
  $script:createdCount++
  if ($script:createFails) { throw 'Root registration failed.' }
  return [pscustomobject]@{ Set = [IntPtr]3; Data = (New-Object Alkaid.SetupApi+SP_DEVINFO_DATA); NeedReboot = $true }
}
function Get-InstanceId([IntPtr]$Set, $Data) { return 'ROOT\ALKAIDUSB\0000' }
function Set-StringProperty([IntPtr]$Set, $Data, [uint32]$Property, [string]$Value) { }
function Remove-DeviceRecord($Record) {
  $script:removed.Add($Record.InstanceId)
  return $script:removeSucceeds
}
function New-Record([string]$Id, [string]$Hardware = $HardwareId, [string]$Name = '') {
  return [pscustomobject]@{
    Set = [IntPtr]2
    Data = (New-Object Alkaid.SetupApi+SP_DEVINFO_DATA)
    InstanceId = $Id
    HardwareId = $Hardware
    Description = $Name
    FriendlyName = $Name
  }
}
function Reset-Case {
  $script:devices = @()
  $script:createdCount = 0
  $script:createFails = $false
  $script:removed = New-Object 'System.Collections.Generic.List[string]'
  $script:removeSucceeds = $true
  [Alkaid.SetupApi]::ParamsOk = $true
  [Alkaid.SetupApi]::InstallerOk = $true
  [Alkaid.SetupApi]::ParamsCalls = 0
  [Alkaid.SetupApi]::InstallerCalls = 0
  [Alkaid.SetupApi]::ClosedSets = 0
}
function Assert-Equal($Expected, $Actual, [string]$Message) {
  if ($Expected -cne $Actual) { throw "$Message (expected=$Expected actual=$Actual)" }
}
function Assert-RootResult($Results, [string]$Id) {
  Assert-Equal 1 @($Results).Count 'Ensure-RootDevice must return one object'
  Assert-Equal $true $Results.RootPresent 'root is present'
  Assert-Equal $Id $Results.InstanceId 'selected root instance'
}
function Assert-Failure([scriptblock]$Body, [string]$Expected) {
  try { & $Body } catch {
    Assert-Equal $Expected $_.Exception.Message 'failure must retain operation and error'
    return
  }
  throw "Expected failure: $Expected"
}
function Test-Case([string]$Name, [scriptblock]$Body) {
  Reset-Case
  & $Body
  $script:passed++
  Write-Host "PASS $Name"
}

$script:passed = 0
Test-Case 'install_without_any_old_device' {
  $root = Ensure-RootDevice
  Assert-RootResult $root 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 1 $script:createdCount 'create a controller on first install'
  Assert-Equal $true $root.NeedReboot 'preserve registration reboot request'
  Assert-Equal 0 ([Alkaid.SetupApi]::ParamsCalls) 'no legacy device to disable'
  $port = [pscustomobject]@{ Healthy = $true; Executable = 'usbip.exe'; ExitCode = 0; Output = ''; MissingVhci = $false }
  $json = Write-Result 'install' $root.RootPresent $root.InstanceId $root.NeedReboot $port | ConvertFrom-Json
  Assert-Equal $true $json.root_present 'entry point can serialize the result'
}
Test-Case 'reuse_canonical_without_legacy_device' {
  $script:devices = @(New-Record 'ROOT\ALKAIDUSB\0000' $HardwareId $RootName)
  Assert-RootResult (Ensure-RootDevice) 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 0 $script:createdCount 'reuse existing controller'
  Assert-Equal 0 ([Alkaid.SetupApi]::ParamsCalls) 'no legacy disable'
}
Test-Case 'reuse_sole_upstream_node_without_disabling_it' {
  $script:devices = @(New-Record $OldInstanceId)
  Assert-RootResult (Ensure-RootDevice) $OldInstanceId
  Assert-Equal 0 ([Alkaid.SetupApi]::ParamsCalls) 'never disable the selected controller'
  Assert-Equal 0 $script:createdCount 'no duplicate controller created'
}
Test-Case 'duplicate_removal_keeps_one_result' {
  $script:devices = @((New-Record 'ROOT\ALKAIDUSB\0000' $HardwareId $RootName), (New-Record 'ROOT\USB\0001'))
  Assert-RootResult (Ensure-RootDevice) 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 'ROOT\USB\0001' $script:removed[0] 'remove only the duplicate'
}
Test-Case 'duplicate_disable_keeps_one_result' {
  $script:removeSucceeds = $false
  $script:devices = @((New-Record 'ROOT\ALKAIDUSB\0000' $HardwareId $RootName), (New-Record 'ROOT\USB\0001'))
  Assert-RootResult (Ensure-RootDevice) 'ROOT\ALKAIDUSB\0000'
}
Test-Case 'unrelated_old_instance_id_is_untouched' {
  $script:devices = @(New-Record $OldInstanceId 'ROOT\OTHER_DEVICE')
  Assert-RootResult (Ensure-RootDevice) 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 0 ([Alkaid.SetupApi]::ParamsCalls) 'an instance ID alone is not ownership'
  Assert-Equal 0 $script:removed.Count 'unrelated node is not a duplicate'
}
Test-Case 'absent_legacy_device_is_a_noop' {
  Disable-LegacyDevice 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 0 ([Alkaid.SetupApi]::ParamsCalls) 'no state operation without a legacy node'
}
Test-Case 'matching_separate_legacy_device_can_be_disabled' {
  $script:devices = @(New-Record $OldInstanceId)
  Disable-LegacyDevice 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 1 ([Alkaid.SetupApi]::ParamsCalls) 'set disable parameters'
  Assert-Equal 1 ([Alkaid.SetupApi]::InstallerCalls) 'perform disable'
}
Test-Case 'class_parameters_failure_names_the_api' {
  [Alkaid.SetupApi]::ParamsOk = $false
  Assert-Failure { Set-DeviceState ([IntPtr]2) (New-Object Alkaid.SetupApi+SP_DEVINFO_DATA) $DICS_DISABLE } 'SetupDiSetClassInstallParams(DIF_PROPERTYCHANGE) failed with Win32 error 203.'
  Assert-Equal 0 ([Alkaid.SetupApi]::InstallerCalls) 'do not call installer after parameter failure'
}
Test-Case 'class_installer_failure_names_the_api' {
  [Alkaid.SetupApi]::InstallerOk = $false
  Assert-Failure { Set-DeviceState ([IntPtr]2) (New-Object Alkaid.SetupApi+SP_DEVINFO_DATA) $DICS_DISABLE } 'SetupDiCallClassInstaller(DIF_PROPERTYCHANGE) failed with Win32 error 203.'
}
Test-Case 'legacy_disable_warning_is_not_a_return_value' {
  [Alkaid.SetupApi]::InstallerOk = $false
  $script:devices = @((New-Record 'ROOT\ALKAIDUSB\0000' $HardwareId $RootName), (New-Record $OldInstanceId))
  $warnings = @()
  $root = & { [CmdletBinding()] param() Ensure-RootDevice } -WarningVariable warnings -WarningAction SilentlyContinue
  Assert-RootResult $root 'ROOT\ALKAIDUSB\0000'
  Assert-Equal 1 $warnings.Count 'retain optional cleanup diagnostic'
  if ($warnings[0] -notmatch 'SetupDiCallClassInstaller.*203') { throw 'missing disable diagnostic' }
}
Test-Case 'root_registration_failure_still_fails' {
  $script:createFails = $true
  Assert-Failure { Ensure-RootDevice } 'Root registration failed.'
}
Test-Case 'native_declarations_match_the_documented_signatures' {
  # The substituted SetupAPI above cannot see the production P/Invoke text, so it is read from the helper itself.
  # ms-newdev-diinstalldevice-20261107: DiInstallDevice takes six parameters (hwndParent, DeviceInfoSet, DeviceInfoData,
  # DriverInfoData, Flags, NeedReboot); five make the call read a stack word that is not an argument.
  # ms-setupapi-setdeviceregistrypropertyw-20261107: the buffers are UTF-16, so the Unicode entry point must be bound;
  # the ANSI one splits "ROOT\USBIP_WIN2\UDE" into one-character hardware IDs and no driver matches (Win32 error 203).
  $source = Get-Content -Raw -LiteralPath $HelperScript
  if ($source -notmatch '(?s)DiInstallDevice\(\s*IntPtr hwndParent,\s*IntPtr DeviceInfoSet,\s*ref SP_DEVINFO_DATA DeviceInfoData,\s*IntPtr DriverInfoData,\s*uint Flags,\s*\[MarshalAs\(UnmanagedType\.Bool\)\] out bool NeedReboot\);') { throw 'DiInstallDevice must declare all six parameters (BOOL out NeedReboot last)' }
  if ($source -notmatch '(?s)DiInstallDevice\(\s*\[IntPtr\]::Zero, \$set, \[ref\]\$data, \[IntPtr\]::Zero, 0, \[ref\]\$needReboot') { throw 'DiInstallDevice call must pass six arguments' }
  foreach ($name in 'SetupDiSetDeviceRegistryProperty', 'SetupDiGetDeviceRegistryProperty') {
    if ($source -notmatch ('\[DllImport\("setupapi\.dll", CharSet = CharSet\.Unicode, SetLastError = true\)\]\s*\[return: MarshalAs\(UnmanagedType\.Bool\)\]\s*public static extern bool ' + $name + '\(')) { throw "$name must use the Unicode entry point" }
  }
}
Write-Host "Results: $script:passed passed; 0 failed; 0 skipped."
