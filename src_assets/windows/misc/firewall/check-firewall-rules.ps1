#requires -version 5.1
param([Parameter(Mandatory)][string]$Program)

$ErrorActionPreference = 'Stop'

function Test-EquivalentRule([object]$Rule, [string]$Program, [int]$Protocol) {
  # Only reuse a rule that grants the same access as the installer. A rule for
  # another program, profile, address, user or port must not suppress creation.
  if ($Rule.Name -ine 'Sunshine' -or $Rule.ApplicationName -ine $Program -or
      -not $Rule.Enabled -or $Rule.Direction -ne 1 -or $Rule.Action -ne 1 -or
      $Rule.Protocol -ne $Protocol -or ($Rule.Profiles -band 7) -ne 7) { return $false }
  if ($Rule.LocalPorts -ne '*' -or $Rule.RemotePorts -ne '*' -or
      $Rule.LocalAddresses -ne '*' -or $Rule.RemoteAddresses -ne '*' -or
      $Rule.InterfaceTypes -ine 'All' -or $Rule.Interfaces -or
      $Rule.EdgeTraversalOptions -ne 0 -or $Rule.SecureFlags -ne 0) { return $false }
  foreach ($value in @($Rule.ServiceName, $Rule.LocalAppPackageId, $Rule.LocalUserOwner,
                       $Rule.LocalUserAuthorizedList, $Rule.RemoteUserAuthorizedList,
                       $Rule.RemoteMachineAuthorizedList)) {
    if (-not [string]::IsNullOrWhiteSpace([string]$value)) { return $false }
  }
  return $true
}

try {
  $rules = (New-Object -ComObject HNetCfg.FwPolicy2).Rules
  $mask = 0
  foreach ($rule in $rules) {
    if (($mask -band 1) -eq 0 -and (Test-EquivalentRule $rule $Program 6)) { $mask = $mask -bor 1 }
    if (($mask -band 2) -eq 0 -and (Test-EquivalentRule $rule $Program 17)) { $mask = $mask -bor 2 }
    if ($mask -eq 3) { break }
  }
  # Reserve 10..13 for valid results so a PowerShell launch failure cannot be
  # mistaken for an existing TCP or UDP rule.
  exit (10 + $mask)
} catch {
  [Console]::Error.WriteLine("Could not inspect existing firewall rules: $($_.Exception.Message)")
  exit 1
}
