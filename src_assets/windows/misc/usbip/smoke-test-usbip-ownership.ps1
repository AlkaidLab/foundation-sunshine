#requires -version 5.1
# Registry/file fixtures only. Never run a driver installer in this suite.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('usbip-ownership-' + [guid]::NewGuid().ToString('N'))
$module = Import-Module (Join-Path $PSScriptRoot 'usbip-package-ownership.psm1') -Force -PassThru -DisableNameChecking
$passed = 0
try {
  $fixtures = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'usbip-ownership-fixtures.json') | ConvertFrom-Json
  foreach ($fixture in $fixtures) {
    $root = Join-Path $scratch $fixture.name
    $hostRoot = Join-Path $root 'host'
    $usbRoot = Join-Path $root 'USBip'
    New-Item -ItemType Directory -Force $usbRoot | Out-Null
    if ($fixture.present) { Set-Content -LiteralPath (Join-Path $usbRoot 'usbip.exe') -Value 'fixture' }
    if ($fixture.extras) {
      Set-Content -LiteralPath (Join-Path $usbRoot 'wusbip.exe') -Value 'fixture'
      New-Item -ItemType Directory -Force (Join-Path $usbRoot 'include'), (Join-Path $usbRoot 'lib') | Out-Null
      Set-Content -LiteralPath (Join-Path $usbRoot 'lib/libusbip.lib') -Value 'fixture'
    }
    $source = Join-Path $hostRoot 'scripts/usbip/USBip-0.9.8.1-x64.exe'
    if ($fixture.legacy) {
      New-Item -ItemType Directory -Force (Split-Path -Parent $source) | Out-Null
      Set-Content -LiteralPath $source -Value 'fixture upstream installer'
    }
    # Simulated ARP registry values. Filesystem probes use actual isolated layouts.
    & $module {
      param($f, $root, $source, $suffix)
      $script:Fixture = $f
      $script:Suffix = $suffix
      $script:FixtureRoot = $root
      $script:Source = $source
      $script:UsbipUninstallBases = @('fixture-registry')
      $script:UsbipSourceHash = if ($f.legacy) { (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash } else { 'fixture-source' }
      function script:Test-Path {
        param($LiteralPath, $ErrorAction)
        if ($LiteralPath -like '*fixture-registry*') {
          if ($LiteralPath -like '*AlkaidLabUSBip') { return $script:Fixture.receipt }
          return $script:Fixture.present -and -not $script:Fixture.receipt
        }
        if ($LiteralPath -like 'HKLM:*') { return $false }
        Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -ErrorAction Stop
      }
      function script:Get-ItemProperty {
        param($LiteralPath, $ErrorAction)
        [pscustomobject]@{ DisplayVersion = $script:Fixture.version; InstallLocation = ($script:FixtureRoot + $script:Suffix);
          InstallLayout = 'usbip-slim-v1'; SourceSHA256 = $script:UsbipSourceHash; UninstallString = '"unins001.exe"' }
      }
    } $fixture $usbRoot $source $(if ($fixture.PSObject.Properties['locationSuffix']) { $fixture.locationSuffix } else { '' })
    $result = Get-UsbipOwnership $usbRoot $hostRoot
    if ($result.ownership -ne $fixture.ownership) { throw "$($fixture.name): expected $($fixture.ownership), got $($result | ConvertTo-Json -Compress)" }
    if ($fixture.name -eq 'observed-full-double-install' -and -not $result.compatible) { throw 'Observed full install must be reusable' }
    $passed++
    Write-Host "PASS ownership/$($fixture.name): $($result.ownership) compatible=$($result.compatible)"
  }
  & $module { function script:Get-UsbipInventory { throw 'simulated registry access denied' } }
  $result = Get-UsbipOwnership $usbRoot $hostRoot
  if ($result.ownership -ne 'external' -or $result.compatible) { throw 'Inspection failure must preserve external install' }
  $passed++
  Write-Host 'PASS ownership/inspection-failure'

  # Execute the production lifecycle with a substituted read-only inventory,
  # and make every mutation command throw. This covers ordering before logs,
  # state, guards and mutexes, not just the classification truth table.
  $scripts = Join-Path $scratch 'entrypoint'
  New-Item -ItemType Directory -Force $scripts | Out-Null
  foreach ($name in 'usbip-package.ps1', 'usbip-package-functions.psm1') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $scripts
  }
  foreach ($compatible in $true, $false) {
    @"
function Get-UsbipOwnership { [pscustomobject]@{ownership='external'; compatible=`$$compatible; reason='fixture'} }
Export-ModuleMember -Function Get-UsbipOwnership
"@ | Set-Content -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1')
    foreach ($action in 'install', 'uninstall', 'remove') {
      & (Join-Path $scripts 'usbip-package.ps1') -Action $action -InstallRoot $usbRoot -HostRoot $hostRoot -State (Join-Path $scripts 'must-not-exist/state.json')
      $expected = if ($action -eq 'install' -and -not $compatible) { 106 } else { 0 }
      if ($LASTEXITCODE -ne $expected) { throw "External $action returned $LASTEXITCODE, expected $expected" }
      if (Test-Path -LiteralPath (Join-Path $scripts 'result-usbip-steps.txt')) { throw 'External install wrote a step log' }
      if (Test-Path -LiteralPath (Join-Path $scripts 'must-not-exist')) { throw 'External install created state' }
      $passed++
      Write-Host "PASS external-entrypoint/$action compatible=$compatible no-state-or-guard"
    }
  }
  $rootStage = Join-Path $scratch 'root-entrypoint'
  New-Item -ItemType Directory -Force (Join-Path $rootStage 'tools'), (Join-Path $rootStage 'scripts') | Out-Null
  $repo = (Get-Item -LiteralPath $PSScriptRoot).Parent.Parent.Parent.Parent.FullName
  Copy-Item -LiteralPath (Join-Path $repo 'tools/usbip-win2-root.ps1') -Destination (Join-Path $rootStage 'tools')
  Copy-Item -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1') -Destination (Join-Path $rootStage 'scripts')
  $oldProgramFiles = $env:ProgramFiles
  try {
    $env:ProgramFiles = $scratch
    foreach ($action in 'install', 'repair', 'uninstall') {
      & (Join-Path $rootStage 'tools/usbip-win2-root.ps1') -Action $action
      if ($LASTEXITCODE -ne 0) { throw "External root helper $action returned $LASTEXITCODE" }
      $passed++
      Write-Host "PASS external-root-entrypoint/$action no-node-mutation"
    }
  } finally { $env:ProgramFiles = $oldProgramFiles }
  Write-Host "USB/IP ownership regressions: $passed passed, 0 skipped, 0 failed"
} finally {
  Remove-Module usbip-package-functions, usbip-package-ownership -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
