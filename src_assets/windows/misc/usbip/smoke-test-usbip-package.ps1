#requires -version 5.1
# Unit regressions use isolated files and mocked OS commands; never install a driver.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('usbip-package-test-' + [guid]::NewGuid().ToString('N'))
$module = Import-Module (Join-Path $PSScriptRoot 'usbip-package-functions.psm1') -Force -PassThru -DisableNameChecking
$passed = 0
$skipped = 0
function Check([string]$Name, [scriptblock]$Test) {
  & $Test
  $script:passed++
  Write-Host "PASS $Name"
}
function Require($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
try {
  $payload = Join-Path $scratch 'payload'
  New-Item -ItemType Directory -Path $payload -Force | Out-Null
  $files = [ordered]@{}
  $names = @('usbip.exe', 'devnode.exe', 'libusbip.dll', 'resources.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'driver/usbip2_ude.inf', 'driver/usbip2_ude.sys', 'driver/usbip2_ude.cat',
    'driver/usbip2_filter.inf', 'driver/usbip2_filter.sys', 'driver/usbip2_filter.cat')
  foreach ($name in $names) {
    $path = Join-Path $payload $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, "fixture $name")
    $files[$name] = @{ sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
  }
  [IO.File]::WriteAllText((Join-Path $payload 'LICENSE'), 'fixture')
  $manifest = @{ schema = 1; layout = 'usbip-slim-v1'; version = '0.9.8.1'; installer_sha256 = 'fixture'; files = $files }
  $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $payload 'payload.json') -Encoding UTF8
  Check 'payload accepts all required original members' { $null = Read-UsbipManifest $payload }
  Check 'modified member fails closed' {
    $path = Join-Path $payload 'usbip.exe'
    $original = [IO.File]::ReadAllBytes($path)
    [IO.File]::WriteAllText($path, 'changed')
    $rejected = $false
    try { $null = Read-UsbipManifest $payload } catch { $rejected = $true }
    Require $rejected 'changed tool was accepted'
    [IO.File]::WriteAllBytes($path, $original)
  }
  Check 'missing catalog fails closed' {
    $path = Join-Path $payload 'driver/usbip2_ude.cat'
    Move-Item -LiteralPath $path -Destination "$path.saved"
    $rejected = $false
    try { $null = Read-UsbipManifest $payload } catch { $rejected = $true }
    Require $rejected 'missing catalog was accepted'
    Move-Item -LiteralPath "$path.saved" -Destination $path
  }
  Check 'missing driver service is not ready' {
    Require (-not (Test-UsbipReady $payload (Read-UsbipManifest $payload))) 'uninstalled fixture was ready'
  }
  if ($env:OS -eq 'Windows_NT') {
    Check 'detach task keeps LocalService and shutdown event triggers' {
      $taskName = 'USBIP-package-test-' + [guid]::NewGuid().ToString('N')
      & $module { param($name); $script:DetachTask = $name } $taskName
      try {
        Set-UsbipDetachTask $payload
        $scheduler = New-Object -ComObject Schedule.Service
        $scheduler.Connect()
        $definition = $scheduler.GetFolder('\').GetTask($taskName).Definition
        $principal = $definition.Principal.UserId
        $sid = if ($principal -like 'S-1-*') { $principal } else {
          (New-Object Security.Principal.NTAccount($principal)).Translate([Security.Principal.SecurityIdentifier]).Value
        }
        Require ($sid -eq 'S-1-5-19') "task service identity changed: $sid"
        Require ($definition.Principal.RunLevel -eq 0) 'task needlessly elevated'
        Require ($definition.Triggers.Count -eq 1) 'shutdown event trigger missing'
        $query = $definition.Triggers.Item(1).Subscription
        Require ($query.Contains('EventID=109') -and $query.Contains('EventID=1074')) 'shutdown event subscription incomplete'
        Require ($definition.Actions.Item(1).Arguments -eq 'detach --all=closeonly') 'detach action changed'
      } finally {
        Set-UsbipDetachTask $payload -Remove
        & $module { $script:DetachTask = 'USBip Detach All On Reboot Or Shutdown' }
      }
    }
  } else {
    $skipped++
    Write-Host 'SKIP detach task OS registration: requires Windows'
  }
  & $module {
    function script:Get-PnpDevice {
      [pscustomobject]@{ InstanceId = 'ROOT\USB\0000' }
      [pscustomobject]@{ InstanceId = 'ROOT\UNRELATED\0001' }
    }
    function script:Get-PnpDeviceProperty {
      param($InstanceId, $KeyName, $ErrorAction)
      [pscustomobject]@{ Data = $(if ($InstanceId -eq 'ROOT\USB\0000') { @('ROOT\USBIP_WIN2\UDE') } else { @('other') }) }
    }
  }
  Check 'upstream root instance is matched through its hardware ID' {
    $nodes = @(Get-UsbipNodes)
    Require ($nodes.Count -eq 1 -and $nodes[0].InstanceId -eq 'ROOT\USB\0000') 'hardware ID was confused with instance ID'
  }
  & $module {
    function script:Get-UsbipOwnership { [pscustomobject]@{ ownership = 'managed' } }
    $script:Calls = [Collections.Generic.List[string]]::new()
    $script:FailDelete = $false
    function script:Set-UsbipDetachTask { param($Root, [switch]$Remove); $script:Calls.Add('task-remove') }
    function script:Get-UsbipOemPackages {
      @([pscustomobject]@{Driver = 'oem10.inf'}, [pscustomobject]@{Driver = 'oem11.inf'})
    }
    function script:Invoke-UsbipCommand {
      param($File, $Arguments)
      $script:Calls.Add(($Arguments -join ' '))
      if ($script:FailDelete -and $Arguments -contains 'oem10.inf') { throw 'simulated first package removal failure' }
    }
  }
  # File-system receipt location avoids all real registry writes on every OS.
  $receipt = Join-Path $scratch 'receipt'
  & $module { param($key); $script:RecordKey = $key } $receipt
  $env:SystemRoot = if ($env:SystemRoot) { $env:SystemRoot } else { $scratch }
  Check 'cleanup continues after a package command fails' {
    & $module { $script:FailDelete = $true }
    $rejected = $false
    try { Remove-UsbipPackage $payload } catch { $rejected = $true }
    Require $rejected 'failed cleanup reported success'
    $calls = @(& $module { $script:Calls.ToArray() })
    Require ($calls -contains '/delete-driver oem11.inf /uninstall /force') 'second driver was not removed'
    Require (Test-Path -LiteralPath (Join-Path $payload 'devnode.exe')) 'recovery tool was deleted'
  }
  Check 'uninstall keeps recovery tools until hub verification completes' {
    & $module { $script:FailDelete = $false }
    Remove-UsbipPackage $payload -KeepFiles
    Require (Test-Path -LiteralPath (Join-Path $payload 'devnode.exe')) 'recovery tool removed before hub verification'
    $null = Read-UsbipManifest $payload
  }
  Check 'cleanup removes only receipt members and preserves unrelated files' {
    & $module { $script:FailDelete = $false }
    [IO.File]::WriteAllText((Join-Path $payload 'unrelated.txt'), 'keep')
    Remove-UsbipPackage $payload
    Require (Test-Path -LiteralPath (Join-Path $payload 'unrelated.txt')) 'unrelated file was deleted'
    Require (-not (Test-Path -LiteralPath (Join-Path $payload 'payload.json'))) 'receipt remains after successful cleanup'
  }
  Check 'external cleanup helpers never reach OS mutations or upstream uninstallers' {
    & $module {
      function script:Get-UsbipOwnership { [pscustomobject]@{ ownership = 'external' } }
      function script:Get-UsbipLegacyEntries { throw 'External upstream uninstaller must not be queried for removal' }
      function script:Set-UsbipDetachTask { throw 'External task must not be changed' }
    }
    Remove-UsbipLegacy $payload $scratch
    Remove-UsbipPackage $payload
  }
  Check 'confirmed legacy ownership uses the registered upstream uninstaller once' {
    & $module {
      $script:LegacyReads = 0
      $script:UninstallCalls = 0
      function script:Get-UsbipOwnership { [pscustomobject]@{ ownership = 'legacy-owned' } }
      function script:Get-UsbipLegacyEntries {
        $script:LegacyReads++
        if ($script:LegacyReads -eq 1) { [pscustomobject]@{ UninstallString = '"C:\USBip\unins001.exe"' } }
      }
      function script:Start-Process {
        param($FilePath, $ArgumentList, [switch]$Wait, [switch]$PassThru)
        if ($FilePath -ne 'C:\USBip\unins001.exe' -or ($ArgumentList -join ' ') -ne '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART') { throw 'Unexpected legacy uninstall command' }
        $script:UninstallCalls++
        [pscustomobject]@{ ExitCode = 0 }
      }
    }
    Remove-UsbipLegacy $payload $scratch
    Require ((& $module { $script:UninstallCalls }) -eq 1) 'Legacy uninstall count changed'
    Require ((Get-UsbipRestartCode) -eq 3010) 'Legacy takeover must report restart'
  }
  Write-Host "USB/IP package regressions: $passed passed, $skipped skipped, 0 failed"
} finally {
  Remove-Module $module -Force
  if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
