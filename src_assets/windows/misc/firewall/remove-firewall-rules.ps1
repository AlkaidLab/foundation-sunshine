#requires -version 5.1
param([Parameter(Mandatory)][string]$Program)

$ErrorActionPreference = 'Stop'

function Test-Empty([object]$Value) {
  if ($null -eq $Value) { return $true }
  if ($Value -is [array]) { return $Value.Count -eq 0 }
  return [string]::IsNullOrWhiteSpace([string]$Value)
}

function Get-FilterIndex([string]$Command, [hashtable]$Ids) {
  $index = @{}
  foreach ($filter in @(& $Command -All -PolicyStore PersistentStore -ErrorAction Stop)) {
    $id = [string]$filter.InstanceID
    if ($Ids.ContainsKey($id)) { $index[$id] = $filter }
  }
  return $index
}

try {
  # Look only in the local persistent policy. Group Policy and rules for other
  # installations do not belong to this uninstaller.
  try {
    $applications = @(Get-NetFirewallApplicationFilter -Program $Program -PolicyStore PersistentStore -ErrorAction Stop)
  } catch {
    if ($_.CategoryInfo.Category -eq 'ObjectNotFound') { exit 0 }
    throw
  }

  $ids = @($applications | Where-Object { $_.Program -ieq $Program -and (Test-Empty $_.Package) } |
    ForEach-Object { [string]$_.InstanceID } | Select-Object -Unique)
  if ($ids.Count -eq 0) { exit 0 }

  $rules = @(Get-NetFirewallRule -Name $ids -PolicyStore PersistentStore -ErrorAction Stop | Where-Object {
    $_.DisplayName -ieq 'Sunshine' -and $_.Name -match '^\{[0-9a-fA-F-]{36}\}$' -and
      $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' -and
      $_.Profile -eq 'Any' -and $_.EdgeTraversalPolicy -eq 'Block' -and
      $_.PolicyStoreSourceType -eq 'Local' -and -not $_.LocalOnlyMapping -and -not $_.LooseSourceMapping -and
      (Test-Empty $_.Description) -and (Test-Empty $_.Group) -and (Test-Empty $_.Owner) -and
      (Test-Empty $_.PackageFamilyName) -and (Test-Empty $_.PolicyAppId) -and
      (Test-Empty $_.Platforms) -and (Test-Empty $_.RemoteDynamicKeywordAddresses)
  })
  if ($rules.Count -eq 0) { exit 0 }

  # Fetch the filter collections once rather than issuing several WMI queries
  # for every historical rule. Match each collection by the rule's unique ID.
  $idSet = @{}
  foreach ($rule in $rules) { $idSet[[string]$rule.Name] = $true }
  $ports = Get-FilterIndex 'Get-NetFirewallPortFilter' $idSet
  $addresses = Get-FilterIndex 'Get-NetFirewallAddressFilter' $idSet
  $interfaces = Get-FilterIndex 'Get-NetFirewallInterfaceFilter' $idSet
  $interfaceTypes = Get-FilterIndex 'Get-NetFirewallInterfaceTypeFilter' $idSet
  $services = Get-FilterIndex 'Get-NetFirewallServiceFilter' $idSet
  $security = Get-FilterIndex 'Get-NetFirewallSecurityFilter' $idSet

  $remove = @()
  foreach ($rule in $rules) {
    $id = [string]$rule.Name
    if (-not $ports.ContainsKey($id) -or -not $addresses.ContainsKey($id) -or
        -not $interfaces.ContainsKey($id) -or -not $interfaceTypes.ContainsKey($id) -or
        -not $services.ContainsKey($id) -or -not $security.ContainsKey($id)) {
      throw "Could not inspect all firewall filters for rule $id."
    }
    $port = $ports[$id]
    $address = $addresses[$id]
    $interface = $interfaces[$id]
    $interfaceType = $interfaceTypes[$id]
    $service = $services[$id]
    $securityFilter = $security[$id]
    if ($port.Protocol -notin @('TCP', 'UDP') -or $port.LocalPort -ne 'Any' -or
        $port.RemotePort -ne 'Any' -or $port.DynamicTarget -ne 'Any' -or
        $port.DynamicTransport -ne 'Any' -or $address.LocalAddress -ne 'Any' -or
        $address.RemoteAddress -ne 'Any' -or $interface.InterfaceAlias -ne 'Any' -or
        $interfaceType.InterfaceType -ne 'Any' -or $service.Service -ne 'Any' -or
        $securityFilter.Authentication -ne 'NotRequired' -or $securityFilter.Encryption -ne 'NotRequired' -or
        $securityFilter.LocalUser -ne 'Any' -or $securityFilter.RemoteUser -ne 'Any' -or
        $securityFilter.RemoteMachine -ne 'Any' -or $securityFilter.OverrideBlockRules) { continue }
    $remove += $id
  }

  if ($remove.Count -gt 0) {
    Remove-NetFirewallRule -Name $remove -PolicyStore PersistentStore -ErrorAction Stop
    Write-Host "Removed $($remove.Count) default Sunshine firewall rule(s)."
  }
  exit 0
} catch {
  [Console]::Error.WriteLine("Could not remove Sunshine firewall rules: $($_.Exception.Message)")
  exit 1
}
