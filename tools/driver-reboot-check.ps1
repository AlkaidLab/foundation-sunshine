#requires -version 5.1
<#
  Asks Windows which of our drivers still wait for a restart: DEVPKEY_Device_IsRebootRequired, or the
  problem code CM_PROB_NEED_RESTART (14), on the devices of ZakoDisplay (vdd), ViGEmBus (vigem),
  the virtual mouse (vmouse) and usbip-win2 (usbip). Writes the names, comma separated, to -Out.
  Read-only: it changes nothing.
#>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'SilentlyContinue'
$map = [ordered]@{ vdd = 'ZakoVDD'; vigem = 'ViGEmBus'; vmouse = 'ZakoVirtualMouse'; usbip = 'USBIP_WIN2' }
$names = foreach ($name in $map.Keys) {
  $pattern = $map[$name]
  $hit = Get-PnpDevice -PresentOnly | Where-Object { ($_.InstanceId -match $pattern) -or (($_.HardwareID -join ';') -match $pattern) } | Where-Object {
    $reboot = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_IsRebootRequired').Data
    $problem = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
    ($reboot -eq $true) -or ($problem -eq 14)
  }
  if ($hit) { $name }
}
Set-Content -LiteralPath $Out -Value ($names -join ',') -Encoding ASCII
exit 0
