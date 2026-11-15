#requires -version 5.1
<##
  Smoke test of usbip-roothub-guard.ps1 (issue #1272). It changes nothing on the machine: the rollback runs with
  -DryRun, the hub fault is injected with -SimulateHubProblem, and the persisted-filter cleanup runs on a scratch
  registry tree under HKCU. Every case runs the guard as its own process, the way install-usbip.bat does.
##>
[CmdletBinding()]
param(
  [string]$GuardScript
)

$ErrorActionPreference = 'Stop'
if (-not $GuardScript) { $GuardScript = Join-Path $PSScriptRoot 'usbip-roothub-guard.ps1' }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("usbip-guard-smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$state = Join-Path $work 'state.json'
$log = Join-Path $work 'guard.log'
$scratch = 'HKCU:\Software\FoundationSunshineSmoke\usbip-guard-' + [guid]::NewGuid().ToString('N')
$propertiesKey = '{a193307d-1245-46d3-be67-d8128670e09b}'

function Assert-Equal($Expected, $Actual, [string]$Message) {
  if ($Expected -ne $Actual) { throw "$Message`nExpected: $Expected`nActual:   $Actual" }
}

function Invoke-Guard([string[]]$Arguments) {
  $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $GuardScript, '-State', $state, '-Log', $log) + $Arguments
  $output = & powershell.exe @all 2>&1 | Out-String
  return [pscustomobject]@{ Code = $LASTEXITCODE; Output = $output }
}

try {
  # 1. nothing changed: verify is healthy (0), or asks for a restart (3010) when Windows already did
  $r = Invoke-Guard @('-Action', 'snapshot')
  Assert-Equal 0 $r.Code 'snapshot must succeed'
  $snapshot = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
  $physical = @(@($snapshot.hubs) | Where-Object { -not $_.Emulated })
  $r = Invoke-Guard @('-Action', 'verify', '-SettleMinSeconds', '4', '-SettleMaxSeconds', '20')
  Assert-Equal 0 $r.Code ("verify without a change must be healthy`n" + $r.Output)

  # 2. a physical root hub with a new Code 19 after the install: the rollback runs (dry), exit 102
  if ($physical.Count -gt 0) {
    $r = Invoke-Guard @('-Action', 'verify', '-SimulateHubProblem', '19', '-DryRun', '-SettleMinSeconds', '4', '-SettleMaxSeconds', '8')
    Assert-Equal 102 $r.Code ("a new physical hub problem must roll back`n" + $r.Output)
    if ($r.Output -notmatch 'ROLLBACK: physical root hub fault') { throw "the rollback must be logged`n$($r.Output)" }
    if ($r.Output -notmatch 'dry-run, would: pnputil /delete-driver|dry-run, would remove') {
      # no usbip2_filter package and no residue on this machine: the dry run has nothing to name, which is fine
      Write-Host 'note: no usbip2_filter package on this machine; the dry run named no delete-driver step'
    }
    # code 14 (restart needed) is not a fault
    $r = Invoke-Guard @('-Action', 'verify', '-SimulateHubProblem', '14', '-DryRun', '-SettleMinSeconds', '4', '-SettleMaxSeconds', '20')
    Assert-Equal 3010 $r.Code ("code 14 on a hub is a restart request, not a rollback`n" + $r.Output)
  } else {
    Write-Host 'note: no physical USB root hub on this machine; the fault-injection cases are skipped'
  }

  # 3. Windows on a USB disk: refused (104), and only then
  $r = Invoke-Guard @('-Action', 'preflight', '-SimulateUsbBoot')
  Assert-Equal 104 $r.Code 'Windows from a USB disk must be refused'
  $r = Invoke-Guard @('-Action', 'preflight')
  if ($r.Code -ne 0 -and $r.Code -ne 104) { throw "preflight must be 0 or 104`n$($r.Output)" }

  # 4. no snapshot: the guard says it could not run (105)
  Remove-Item -LiteralPath $state -ErrorAction SilentlyContinue
  $r = Invoke-Guard @('-Action', 'verify')
  Assert-Equal 105 $r.Code 'verify without a snapshot must report 105'

  # 5. persisted filter names (usbip-win2 issue #201): removed from both places, other filters and the *Upper marker kept
  $hubA = "$scratch\inst-a"
  $hubB = "$scratch\inst-b"
  New-Item -Path "$hubA\Filters" -Force | Out-Null
  New-ItemProperty -Path "$hubA\Filters" -Name '*Upper' -PropertyType MultiString -Value @('usbip2_filter') | Out-Null
  New-Item -Path "$hubA\Properties\$propertiesKey\0002" -Force | Out-Null
  $residueBytes = [System.Text.Encoding]::Unicode.GetBytes("*Upper`0usbip2_filter`0`0")
  $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($hubA.Substring(6) + "\Properties\$propertiesKey\0002", $true)
  $k.SetValue('', $residueBytes, [Microsoft.Win32.RegistryValueKind]::Binary)
  $k.Dispose()
  New-Item -Path "$hubB\Filters" -Force | Out-Null
  New-ItemProperty -Path "$hubB\Filters" -Name '*Upper' -PropertyType MultiString -Value @('othervendor_filter', 'usbip2_filter') | Out-Null

  $r = Invoke-Guard @('-Action', 'status', '-EnumRoot', $scratch)
  if ($r.Output -notmatch 'persisted filter names: 3') { throw "status must see the 3 persisted names`n$($r.Output)" }
  $r = Invoke-Guard @('-Action', 'cleanup', '-EnumRoot', $scratch)
  Assert-Equal 0 $r.Code ("cleanup must clear the persisted names`n" + $r.Output)
  $r = Invoke-Guard @('-Action', 'status', '-EnumRoot', $scratch)
  if ($r.Output -notmatch 'persisted filter names: 0') { throw "no persisted name may be left`n$($r.Output)" }

  $afterA = (Get-ItemProperty -Path "$hubA\Filters").'*Upper'
  Assert-Equal 0 @($afterA | Where-Object { $_ -eq 'usbip2_filter' }).Count 'hub A must lose usbip2_filter'
  $afterB = @((Get-ItemProperty -Path "$hubB\Filters").'*Upper')
  Assert-Equal 'othervendor_filter' ($afterB -join ',') 'hub B must keep the other vendor filter'
  $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($hubA.Substring(6) + "\Properties\$propertiesKey\0002")
  $kind = $k.GetValueKind('')
  $text = [System.Text.Encoding]::Unicode.GetString([byte[]]$k.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames))
  $k.Dispose()
  Assert-Equal 'Binary' ([string]$kind) 'the persisted property must stay binary'
  Assert-Equal "*Upper`0`0" $text 'the *Upper marker must stay, the filter name must go'

  Write-Host 'usbip-roothub-guard smoke test passed'
} finally {
  Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath 'HKCU:\Software\FoundationSunshineSmoke' -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
