param(
  [ValidateRange(1, 2147483647)]
  [int]$Iterations = 5,
  [ValidateRange(0, 2147483647)]
  [int]$Monitor = 0,
  [ValidateRange(0, 2147483647)]
  [int]$Slots = 3,
  [ValidateRange(0, 2147483647)]
  [int]$DelaySeconds = 3,
  [string]$ProbePath = "",
  [string]$SunshineLog = "C:\Program Files\Sunshine\config\sunshine.log",
  [string]$TriggerMonitorScript = $env:VDD_TRIGGER_MONITOR_SCRIPT,
  [switch]$RestartSunshineService,
  [switch]$RequireSunshineSealedLog
)

$ErrorActionPreference = "Stop"

function Resolve-ProbePath {
  if ($ProbePath) {
    return $ProbePath
  }

  $repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
  $candidate = Join-Path $repoRoot "build\tools\vdd-frame-channel-probe.exe"
  if (Test-Path $candidate) {
    return $candidate
  }

  throw "vdd-frame-channel-probe.exe not found. Build target vdd-frame-channel-probe first, or pass -ProbePath."
}

function Test-IsAdmin {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = [Security.Principal.WindowsPrincipal]::new($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Restart-Sunshine {
  [CmdletBinding(SupportsShouldProcess)]
  param()

  if (-not (Test-IsAdmin)) {
    throw "-RestartSunshineService requires an elevated PowerShell."
  }

  if ($PSCmdlet.ShouldProcess("SunshineService", "Restart service")) {
    Restart-Service -Name SunshineService -Force -ErrorAction Stop
    Start-Sleep -Seconds 4
  }
}

function Invoke-TriggerMonitor {
  if (-not $TriggerMonitorScript) {
    Write-Warning "TriggerMonitorScript not specified; monitor trigger step skipped. Pass -TriggerMonitorScript or set VDD_TRIGGER_MONITOR_SCRIPT."
    return
  }

  if (Test-Path $TriggerMonitorScript) {
    try {
      $triggerOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $TriggerMonitorScript 2>&1 |
        ForEach-Object { $_.ToString() }
      if ($LASTEXITCODE -ne 0) {
        throw "trigger monitor exited with code $LASTEXITCODE. $($triggerOutput -join "`n")"
      }
    }
    catch {
      Write-Warning "trigger monitor failed: $($_.Exception.Message)"
      throw
    }
  }
  else {
    Write-Warning "TriggerMonitorScript not found at '$TriggerMonitorScript'; monitor trigger step skipped."
  }
}

function Invoke-Probe($probe) {
  $oldErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $output = & $probe --open --monitor $Monitor --slots $Slots 2>&1 | ForEach-Object { $_.ToString() }
    $exitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $oldErrorActionPreference
  }

  [pscustomobject]@{
    ExitCode = $exitCode
    Output = ($output -join "`n")
  }
}

function Get-SunshineLogOffset {
  if (-not (Test-Path $SunshineLog)) {
    return 0
  }

  return (Get-Item $SunshineLog).Length
}

function Read-RecentSunshineSignal {
  param(
    [long]$StartOffset
  )

  if (-not (Test-Path $SunshineLog)) {
    return ""
  }

  $patterns = "opened sealed monitor|backend ready|channel=sealed|selection=|meta_seq|generation=|unable to read stable metadata|frame channel open failed|sealed frame channel attach failed|requesting reinit"
  $file = Get-Item $SunshineLog
  $offset = if ($file.Length -ge $StartOffset) { $StartOffset } else { 0 }
  $stream = [System.IO.File]::Open($SunshineLog, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    [void]$stream.Seek($offset, [System.IO.SeekOrigin]::Begin)
    $reader = [System.IO.StreamReader]::new($stream)
    try {
      $content = $reader.ReadToEnd()
    }
    finally {
      $reader.Dispose()
    }
  }
  finally {
    $stream.Dispose()
  }

  $lines = $content -split "`r?`n" | Select-String -Pattern $patterns | Select-Object -Last 80
  return (($lines | ForEach-Object { $_.Line }) -join "`n")
}

$probe = Resolve-ProbePath
$failures = 0

Write-Host "VDD sealed channel stress"
Write-Host "probe=$probe"
Write-Host "iterations=$Iterations monitor=$Monitor slots=$Slots"

for ($i = 1; $i -le $Iterations; ++$i) {
  Write-Host ""
  Write-Host "iteration $i/$Iterations"
  $iterationLogOffset = Get-SunshineLogOffset

  if ($RestartSunshineService) {
    Restart-Sunshine
  }

  Invoke-TriggerMonitor
  Start-Sleep -Seconds $DelaySeconds

  $probeResult = Invoke-Probe $probe
  Write-Host $probeResult.Output

  if ($probeResult.ExitCode -ne 0) {
    ++$failures
    Write-Warning "probe failed with exit code $($probeResult.ExitCode)"
  }
  elseif ($probeResult.Output -notmatch "metadata:" -or
          $probeResult.Output -notmatch "generation=" -or
          $probeResult.Output -notmatch "slot=\d+/\d+") {
    ++$failures
    Write-Warning "probe succeeded but metadata telemetry was incomplete"
  }

  $signals = Read-RecentSunshineSignal $iterationLogOffset
  if ($signals) {
    Write-Host "recent sunshine signals:"
    Write-Host $signals

    if ($signals -match "unable to read stable metadata|frame channel open failed|sealed frame channel attach failed|selection=(open_failed|open_unsupported|attach_failed|sealed_required_failed|caps_failed)") {
      ++$failures
      Write-Warning "Sunshine log contains sealed-channel warning/error signals"
    }
    if ($RequireSunshineSealedLog -and $signals -notmatch "channel=sealed") {
      ++$failures
      Write-Warning "Sunshine log does not contain channel=sealed"
    }
    if ($RequireSunshineSealedLog -and $signals -notmatch "selection=sealed_opened") {
      ++$failures
      Write-Warning "Sunshine log does not contain selection=sealed_opened"
    }
  }
  elseif ($RequireSunshineSealedLog) {
    ++$failures
    Write-Warning "No Sunshine sealed-channel log signals found"
  }
}

if ($failures -ne 0) {
  throw "VDD sealed channel stress finished with $failures failure(s)."
}

Write-Host ""
Write-Host "VDD sealed channel stress passed."
