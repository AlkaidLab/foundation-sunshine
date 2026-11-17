#requires -version 5.1
# Read-only acceptance of an existing, compatible externally managed install.
# Scratch contains only evidence and a staged copy of the installed script layout.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$EvidenceRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $EvidenceRoot | Out-Null
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$stage = Join-Path $EvidenceRoot 'staged'
$scripts = Join-Path $stage 'scripts'
New-Item -ItemType Directory -Force $scripts, (Join-Path $stage 'tools') | Out-Null
Copy-Item (Join-Path $repo 'src_assets/windows/misc/usbip/usbip-package*') $scripts
Copy-Item (Join-Path $repo 'tools/usbip-win2-root.ps1') (Join-Path $stage 'tools')
Import-Module (Join-Path $scripts 'usbip-package-ownership.psm1') -Force -DisableNameChecking
$root = Join-Path $env:ProgramFiles 'USBip'
$owner = Get-UsbipOwnership $root $stage
if ($owner.ownership -ne 'external' -or -not $owner.compatible) { throw 'This check requires a compatible externally managed installation; no lifecycle action was run' }

function Snapshot([string]$Name) {
  $dir = Join-Path $EvidenceRoot $Name
  New-Item -ItemType Directory -Force $dir | Out-Null
  $paths = @($root)
  # usbip-slim-dism-20261117, Online/All; inventory only, never add/delete drivers.
  $packages = @(Get-WindowsDriver -Online -All | Where-Object { [IO.Path]::GetFileName($_.OriginalFileName) -in 'usbip2_ude.inf', 'usbip2_filter.inf' } | Sort-Object Driver | Select-Object Driver, OriginalFileName, Version, ProviderName, BootCritical)
  $packages | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $dir 'drivers.json')
  foreach ($package in $packages) { $paths += Split-Path -Parent $package.OriginalFileName }
  $services = @(Get-CimInstance Win32_SystemDriver | Where-Object { $_.Name -in 'usbip2_ude', 'usbip2_filter', 'usbip2_vhci' } | Sort-Object Name | Select-Object Name, State, StartMode, PathName, ServiceType)
  $services | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $dir 'services.json')
  $files = @(foreach ($path in $paths | Sort-Object -Unique) {
    foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File -Force | Sort-Object FullName) {
      [pscustomobject]@{ path=$file.FullName; length=$file.Length; modified=$file.LastWriteTimeUtc.Ticks; attributes=[string]$file.Attributes; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
  })
  $files | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dir 'files.json')
  $keys = @('HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1',
    'HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1',
    'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AlkaidLabUSBip',
    'HKLM\SYSTEM\CurrentControlSet\Services\usbip2_ude', 'HKLM\SYSTEM\CurrentControlSet\Services\usbip2_filter',
    'HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\ALKAIDUSB', 'HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\USB',
    'HKLM\SYSTEM\CurrentControlSet\Control\Class\{36fc9e60-c465-11cf-8056-444553540000}')
  $index = 0
  foreach ($key in $keys) {
    # Missing keys are recorded as missing, not silently omitted on one side.
    $target = Join-Path $dir ("registry-$index.reg")
    $providerPath = 'Registry::' + $key.Replace('HKLM\', 'HKEY_LOCAL_MACHINE\')
    if (Test-Path -LiteralPath $providerPath) {
      & reg.exe export $key $target /y | Out-Null
      if ($LASTEXITCODE -ne 0) { throw 'Registry inventory failed' }
    } else { Set-Content -LiteralPath $target -Value 'absent' }
    $index++
  }
  return $files.Count
}

$count = Snapshot before
$package = Join-Path $scripts 'usbip-package.ps1'
$beforeStatus = & $package -Action status -InstallRoot $root -HostRoot $stage | ConvertFrom-Json
if (-not $beforeStatus.ready -or $beforeStatus.ownership -ne 'external') { throw 'External transport did not pass pinned compatibility/readiness checks' }
$results = @()
foreach ($action in 'install', 'uninstall', 'remove') {
  & $package -Action $action -InstallRoot $root -HostRoot $stage
  if ($LASTEXITCODE -ne 0) { throw "External $action returned $LASTEXITCODE" }
  $results += "package/$action=0"
}
foreach ($action in 'install', 'repair', 'uninstall') {
  & (Join-Path $stage 'tools/usbip-win2-root.ps1') -Action $action
  if ($LASTEXITCODE -ne 0) { throw "External root $action returned $LASTEXITCODE" }
  $results += "root/$action=0"
}
& (Join-Path $root 'usbip.exe') port | Set-Content (Join-Path $EvidenceRoot 'port.txt')
if ($LASTEXITCODE -ne 0) { throw 'External USB/IP port probe failed' }
$afterStatus = & $package -Action status -InstallRoot $root -HostRoot $stage | ConvertFrom-Json
if (-not $afterStatus.ready -or $afterStatus.ownership -ne 'external') { throw 'External transport readiness changed' }
$afterCount = Snapshot after
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $EvidenceRoot 'before') -File) {
  $other = Join-Path (Join-Path $EvidenceRoot 'after') $file.Name
  if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $other).Hash) { throw "Before/after mismatch: $($file.Name)" }
}
[pscustomobject]@{ scenario='external-full'; ownership='external'; ready=$afterStatus.ready; files_before=$count; files_after=$afterCount; changed_categories=0; actions=$results; mutating_scenarios='not run'; hardware_functions='not verified' } | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $EvidenceRoot 'result.json')
