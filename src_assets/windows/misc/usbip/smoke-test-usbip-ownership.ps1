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
      foreach ($name in 'devnode.exe', 'libusbip.dll') { Set-Content -LiteralPath (Join-Path $usbRoot $name) -Value 'fixture' }
      New-Item -ItemType Directory -Force (Join-Path $usbRoot 'include'), (Join-Path $usbRoot 'lib') | Out-Null
      Set-Content -LiteralPath (Join-Path $usbRoot 'lib/libusbip.lib') -Value 'fixture'
    }
    # The upstream setup leaves its own uninstaller and uninstall data in the install directory.
    if ($fixture.present -and -not $fixture.receipt) {
      foreach ($name in 'unins000.exe', 'unins000.dat') { Set-Content -LiteralPath (Join-Path $usbRoot $name) -Value 'fixture' }
    }
    $source = Join-Path $hostRoot 'scripts/usbip/USBip-0.9.8.1-x64.exe'
    if ($fixture.legacy) {
      New-Item -ItemType Directory -Force (Split-Path -Parent $source) | Out-Null
      Set-Content -LiteralPath $source -Value 'fixture upstream installer'
    }
    # The earlier release installer's evidence: a guard transaction in the host log, and when the setup created its files.
    $variant = if ($fixture.PSObject.Properties['proofVariant']) { $fixture.proofVariant } else { '' }
    $hubs = 'root hubs: USB\ROOT_HUB30\5&1DACDAE8&0&0 status=OK problem=0'
    $snapshot = "2026-11-16 10:00:02 [usbip-guard] snapshot: $hubs; filter packages: " + $(if ($variant -eq 'filter-before') { 'oem12.inf' } else { '' })
    $logLines = @("2026-11-16 10:00:00 [usbip-guard] preflight ok. $hubs", $snapshot)
    if ($variant -eq 'rollback-inside') { $logLines += "2026-11-16 10:00:20 [usbip-guard] ROLLBACK: physical root hub fault after the usbip-win2 install" }
    if ($variant -ne 'open-window') { $logLines += "2026-11-16 10:00:30 [usbip-guard] verify ok. $hubs" }
    if ($variant -ne '' -and $variant -ne 'no-log') {
      New-Item -ItemType Directory -Force (Join-Path $hostRoot 'scripts') | Out-Null
      # As Windows PowerShell 5.1 writes it (Add-Content -Encoding UTF8): byte order mark, CRLF.
      [IO.File]::WriteAllText((Join-Path $hostRoot 'scripts/usbip-roothub-guard.log'), (($logLines -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($true)))
    }
    if ($variant -eq 'two-uninstallers') { Set-Content -LiteralPath (Join-Path $usbRoot 'unins001.exe') -Value 'fixture' }
    $created = [datetime]'2026-11-16 10:00:10'
    if ($variant -eq 'before-window') { $created = [datetime]'2026-11-16 09:00:00' }
    if ($variant -eq 'after-window') { $created = [datetime]'2026-11-16 11:00:00' }
    $written = if ($variant -eq 'written-outside') { [datetime]'2026-11-17 08:00:00' } else { $created.AddSeconds(2) }
    $inno = [ordered]@{ 'Inno Setup: Selected Components' = 'main,client,gui,sdk,pdb'; 'Inno Setup: Deselected Components' = ''; InstallDate = '20261116' }
    if ($variant -eq 'custom-components') { $inno['Inno Setup: Selected Components'] = 'main,client' }
    if ($variant -eq 'deselected') { $inno['Inno Setup: Deselected Components'] = 'pdb' }
    if ($variant -eq 'other-day') { $inno['InstallDate'] = '20261115' }
    if ($variant -eq 'no-date') { $inno.Remove('InstallDate') }
    # Simulated ARP registry values. Filesystem probes use actual isolated layouts.
    & $module {
      param($f, $root, $source, $suffix, $inno, $created, $written)
      $script:Fixture = $f
      $script:Suffix = $suffix
      $script:FixtureRoot = $root
      $script:Source = $source
      $script:Inno = $inno
      $script:Created = $created
      $script:Written = $written
      $script:UsbipUninstallBases = if ($f.PSObject.Properties['twoViews']) { @('fixture-registry-a', 'fixture-registry-b') } else { @('fixture-registry') }
      $script:UsbipSourceHash = if ($f.legacy) { (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash } else { 'fixture-source' }
      function script:Get-UsbipFileTimes {
        param([string]$Path)
        [pscustomobject]@{ Created = $script:Created; Written = $(if ($Path -like '*.dat') { $script:Written } else { $script:Created }) }
      }
      function script:Test-Path {
        param($LiteralPath, $PathType, $ErrorAction)
        if ($LiteralPath -like '*fixture-registry*') {
          if ($LiteralPath -like '*AlkaidLabUSBip') { return $script:Fixture.receipt }
          return $script:Fixture.present -and -not $script:Fixture.receipt
        }
        if ($LiteralPath -like 'HKLM:*') { return $false }
        if ($PathType) { return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType -ErrorAction Stop }
        Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -ErrorAction Stop
      }
      function script:Get-ItemProperty {
        param($LiteralPath, $ErrorAction)
        $record = [pscustomobject]@{ DisplayVersion = $script:Fixture.version; InstallLocation = ($script:FixtureRoot + $script:Suffix);
          InstallLayout = 'usbip-slim-v1'; SourceSHA256 = $script:UsbipSourceHash
          UninstallString = '"' + $(if ($script:Fixture.PSObject.Properties['uninstallerElsewhere']) { $script:FixtureRoot + '-other' } else { $script:FixtureRoot }) + '\unins000.exe" /LOG' }
        if ($LiteralPath -notlike '*AlkaidLabUSBip') { foreach ($name in $script:Inno.Keys) { $record | Add-Member -NotePropertyName $name -NotePropertyValue $script:Inno[$name] } }
        $record
      }
    } $fixture $usbRoot $source $(if ($fixture.PSObject.Properties['locationSuffix']) { $fixture.locationSuffix } else { '' }) $inno $created $written
    $result = Get-UsbipOwnership $usbRoot $hostRoot
    if ($result.ownership -ne $fixture.ownership) { throw "$($fixture.name): expected $($fixture.ownership), got $($result | ConvertTo-Json -Compress)" }
    if ($fixture.PSObject.Properties['reason'] -and $result.reason -ne $fixture.reason) { throw "$($fixture.name): expected reason $($fixture.reason), got $($result | ConvertTo-Json -Compress)" }
    if ($fixture.PSObject.Properties['missing'] -and (@($result.proofMissing | Sort-Object) -join ',') -ne (@($fixture.missing | Sort-Object) -join ',')) {
      throw "$($fixture.name): expected missing proof signals [$(@($fixture.missing) -join ',')], got [$(@($result.proofMissing) -join ',')]"
    }
    if ($result.replaceable -ne $fixture.replaceable) { throw "$($fixture.name): expected replaceable=$($fixture.replaceable), got $($result | ConvertTo-Json -Compress)" }
    if ($fixture.name -eq 'observed-full-double-install' -and -not $result.compatible) { throw 'Observed full install must be reusable' }
    # A confirmed replacement takes over exactly the replaceable installs and changes nothing else.
    $confirmed = Get-UsbipOwnership $usbRoot $hostRoot -ConfirmedReplace
    $expected = if ($fixture.replaceable) { 'legacy-owned' } else { $fixture.ownership }
    if ($confirmed.ownership -ne $expected) { throw "$($fixture.name): confirmed replacement expected $expected, got $($confirmed | ConvertTo-Json -Compress)" }
    $passed++
    Write-Host "PASS ownership/$($fixture.name): $($result.ownership) ($($result.reason)) compatible=$($result.compatible) replaceable=$($result.replaceable) confirmed=$($confirmed.ownership)"
  }
  # Leftovers with no registration at all (another tool's usbip-win2, removed by its own uninstaller) are external and cannot be
  # installed over; the reason must say that nothing is registered, not that the version is wrong.
  $inventory = [pscustomobject]@{ Present = $true; Entries = @(); Receipt = $null; Extras = $false; LegacyPayload = $false; Root = 'C:\fixture\USBip'; Proof = $null }
  $orphan = Resolve-UsbipOwnership $inventory
  if ($orphan.ownership -ne 'external' -or $orphan.compatible -or $orphan.reason -ne 'no-registration' -or $orphan.replaceable) { throw "Leftovers without a registration: $($orphan | ConvertTo-Json -Compress)" }
  $stale = [pscustomobject]@{ Present = $true; Entries = @([pscustomobject]@{ DisplayVersion = '0.9.7.7'; InstallLocation = 'C:\fixture\USBip\'; UninstallString = '"C:\fixture\USBip\unins000.exe" /LOG' }); Receipt = $null; Extras = $false; LegacyPayload = $false; Root = 'C:\fixture\USBip'; Proof = $null }
  if ((Resolve-UsbipOwnership $stale).reason -ne 'unsupported-version') { throw 'A registered other version keeps its reason' }
  $passed++
  Write-Host 'PASS ownership/no-registration-leftovers'

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
  # Ownership is decided again under the lifecycle lock: a first read of managed followed by a second read of external
  # (another process changed the installation while this one waited) must change nothing.
  $counter = Join-Path $scripts 'ownership-reads.txt'
  foreach ($action in 'install', 'uninstall') {
    Remove-Item -LiteralPath $counter -Force -ErrorAction SilentlyContinue
    @"
function Get-UsbipOwnership {
  `$n = if (Test-Path -LiteralPath '$counter') { [int](Get-Content -LiteralPath '$counter') } else { 0 }
  Set-Content -LiteralPath '$counter' -Value (`$n + 1)
  if (`$n -eq 0) { [pscustomobject]@{ownership='managed'; compatible=`$true; reason='fixture'} }
  else { [pscustomobject]@{ownership='external'; compatible=`$true; reason='fixture'} }
}
Export-ModuleMember -Function Get-UsbipOwnership
"@ | Set-Content -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1')
    & (Join-Path $scripts 'usbip-package.ps1') -Action $action -InstallRoot $usbRoot -HostRoot $hostRoot -State (Join-Path $scripts 'must-not-exist/state.json')
    if ($LASTEXITCODE -ne 0) { throw "Changed-under-lock $action returned $LASTEXITCODE" }
    if ([int](Get-Content -LiteralPath $counter) -ne 2) { throw "Changed-under-lock $action did not read ownership again under the lock" }
    if (Test-Path -LiteralPath (Join-Path $scripts 'result-usbip-steps.txt')) { throw "Changed-under-lock $action wrote a step log" }
    if (Test-Path -LiteralPath (Join-Path $scripts 'must-not-exist')) { throw "Changed-under-lock $action created state" }
    $passed++
    Write-Host "PASS ownership-under-lock/$action external-after-wait no-state-or-guard"
  }
  # `replace` is the one action that may take over an install we did not prove to be ours, and only when ownership
  # resolves to legacy-owned with the confirmation. In every other case it refuses with 107 and changes nothing.
  foreach ($owner in 'external', 'managed', 'absent') {
    @"
function Get-UsbipOwnership { param([string]`$Root, [string]`$HostRoot, [switch]`$ConfirmedReplace)
  [pscustomobject]@{ownership='$owner'; compatible=`$true; reason='fixture'; replaceable=`$false; version='0.9.8.1'} }
Export-ModuleMember -Function Get-UsbipOwnership
"@ | Set-Content -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1')
    & (Join-Path $scripts 'usbip-package.ps1') -Action replace -InstallRoot $usbRoot -HostRoot $hostRoot -State (Join-Path $scripts 'must-not-exist/state.json')
    if ($LASTEXITCODE -ne 107) { throw "Replace of a $owner install returned $LASTEXITCODE, expected 107" }
    if (Test-Path -LiteralPath (Join-Path $scripts 'result-usbip-steps.txt')) { throw 'Refused replace wrote a step log' }
    if (Test-Path -LiteralPath (Join-Path $scripts 'must-not-exist')) { throw 'Refused replace created state' }
    $passed++
    Write-Host "PASS replace-entrypoint/$owner refused with 107, no-state-or-guard"
  }
  # The status of an external install tells the panel whether the person may ask for a replacement.
  @"
function Get-UsbipOwnership { param([string]`$Root, [string]`$HostRoot, [switch]`$ConfirmedReplace)
  [pscustomobject]@{ownership='external'; compatible=`$true; reason='gui-or-sdk'; replaceable=`$true; version='0.9.8.1'} }
Export-ModuleMember -Function Get-UsbipOwnership
"@ | Set-Content -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1')
  $status = (& (Join-Path $scripts 'usbip-package.ps1') -Action status -InstallRoot $usbRoot -HostRoot $hostRoot -State (Join-Path $scripts 'must-not-exist/state.json') | Out-String) | ConvertFrom-Json
  if ($status.ownership -ne 'external' -or $status.replaceable -ne $true -or $status.version -ne '0.9.8.1' -or $status.reason -ne 'gui-or-sdk') { throw "Status lacks the replace facts: $($status | ConvertTo-Json -Compress)" }
  $passed++
  Write-Host 'PASS replace-entrypoint/status reports replaceable and version'
  # Under the lock the confirmation is decided again, with the confirmation: if the install changed while this process
  # waited (first read replaceable, second read not), nothing is touched and the result is 107.
  $counter = Join-Path $scripts 'ownership-reads.txt'
  Remove-Item -LiteralPath $counter -Force -ErrorAction SilentlyContinue
  @"
function Get-UsbipOwnership { param([string]`$Root, [string]`$HostRoot, [switch]`$ConfirmedReplace)
  `$n = if (Test-Path -LiteralPath '$counter') { @(Get-Content -LiteralPath '$counter').Count } else { 0 }
  Add-Content -LiteralPath '$counter' -Value "confirmed=`$(`$ConfirmedReplace.IsPresent)"
  if (`$n -eq 0 -and `$ConfirmedReplace) { [pscustomobject]@{ownership='legacy-owned'; compatible=`$true; reason='user-confirmed-replace'; replaceable=`$false; version='0.9.8.1'} }
  else { [pscustomobject]@{ownership='external'; compatible=`$true; reason='fixture'; replaceable=`$false; version='0.9.8.1'} }
}
Export-ModuleMember -Function Get-UsbipOwnership
"@ | Set-Content -LiteralPath (Join-Path $scripts 'usbip-package-ownership.psm1')
  & (Join-Path $scripts 'usbip-package.ps1') -Action replace -InstallRoot $usbRoot -HostRoot $hostRoot -State (Join-Path $scripts 'must-not-exist/state.json')
  if ($LASTEXITCODE -ne 107) { throw "Replace that lost its eligibility under the lock returned $LASTEXITCODE, expected 107" }
  $reads = @(Get-Content -LiteralPath $counter)
  if ($reads.Count -ne 2 -or @($reads | Where-Object { $_ -ne 'confirmed=True' }).Count -ne 0) { throw "Replace must read ownership twice, both with the confirmation: $($reads -join ';')" }
  if (Test-Path -LiteralPath (Join-Path $scripts 'result-usbip-steps.txt')) { throw 'Replace that lost eligibility wrote a step log' }
  if (Test-Path -LiteralPath (Join-Path $scripts 'must-not-exist')) { throw 'Replace that lost eligibility created state' }
  $passed++
  Write-Host 'PASS replace-entrypoint/eligibility-lost-under-lock refused with 107, both reads confirmed'
  Write-Host "USB/IP ownership regressions: $passed passed, 0 skipped, 0 failed"
} finally {
  Remove-Module usbip-package-functions, usbip-package-ownership -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
