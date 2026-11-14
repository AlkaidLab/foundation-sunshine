param(
  [Parameter(Mandatory = $true)]
  [string] $Root
)

# Authority: [installer-inno-script-functions-20261107] for temporary extraction;
# [installer-ms-terminateprocess-20261113] for process termination boundaries.
# The root scope is the old component directory. Do not uninstall the shared
# USB/IP driver: the native DualSense path uses it after the upgrade.
$ErrorActionPreference = 'Continue'
$rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'

function Test-OwnedPath {
  param([AllowNull()][string] $Path)
  if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
  try {
    return [IO.Path]::GetFullPath($Path).StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)
  } catch {
    return $false
  }
}

function Get-ExecutablePathFromCommand {
  param([AllowNull()][string] $Command)
  if ([string]::IsNullOrWhiteSpace($Command)) { return $null }
  if ($Command -match '^\s*"([^"]+)"') { return $Matches[1] }
  return ($Command -split '\s+', 2)[0]
}

Get-CimInstance Win32_Process -Filter "Name='Sunshine.Ds5Sidecar.exe'" |
  Where-Object { Test-OwnedPath $_.ExecutablePath } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Get-CimInstance Win32_Service |
  Where-Object { Test-OwnedPath (Get-ExecutablePathFromCommand $_.PathName) } |
  ForEach-Object {
    Stop-Service -Name $_.Name -Force -ErrorAction SilentlyContinue
    & sc.exe delete $_.Name | Out-Null
  }

Get-ScheduledTask -ErrorAction SilentlyContinue |
  Where-Object {
    $_.Actions | Where-Object { Test-OwnedPath (Get-ExecutablePathFromCommand $_.Execute) }
  } |
  ForEach-Object {
    Unregister-ScheduledTask -TaskName $_.TaskName -TaskPath $_.TaskPath -Confirm:$false -ErrorAction SilentlyContinue
  }

Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |
  Where-Object { Test-OwnedPath $_.Program } |
  ForEach-Object {
    $_ | Get-NetFirewallRule -ErrorAction SilentlyContinue |
      Remove-NetFirewallRule -ErrorAction SilentlyContinue
  }

Write-Output "Legacy DualSense integrations owned by $rootFull were swept"
