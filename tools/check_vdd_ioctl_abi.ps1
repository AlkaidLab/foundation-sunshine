param(
  [string]$SunshineHeader = "",
  [string]$DriverHeader = $env:VDD_DRIVER_HEADER
)

$ErrorActionPreference = "Stop"

if (-not $SunshineHeader) {
  $repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
  $SunshineHeader = Join-Path $repoRoot "src\display_device\vdd_control_ioctl.h"
}

if (-not $DriverHeader) {
  throw "DriverHeader not specified. Pass -DriverHeader <path> or set `$env:VDD_DRIVER_HEADER."
}

if (-not (Test-Path $SunshineHeader)) {
  throw "Sunshine ABI header not found: $SunshineHeader"
}
if (-not (Test-Path $DriverHeader)) {
  throw "Driver ABI header not found: $DriverHeader"
}

$sunshineText = [IO.File]::ReadAllText($SunshineHeader).Replace("`r`n", "`n")
$driverText = [IO.File]::ReadAllText($DriverHeader).Replace("`r`n", "`n")
$sha256 = [Security.Cryptography.SHA256]::Create()
$sunshineHash = [BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($sunshineText))).Replace("-", "")
$driverHash = [BitConverter]::ToString($sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($driverText))).Replace("-", "")

Write-Host "Sunshine ABI: $SunshineHeader"
Write-Host "Driver ABI:   $DriverHeader"
Write-Host "Sunshine SHA256: $sunshineHash"
Write-Host "Driver SHA256:   $driverHash"

if ($sunshineText -ne $driverText) {
  Write-Error "VDD IOCTL ABI headers differ. Sync src/display_device/vdd_control_ioctl.h and Common/Include/vdd_control_ioctl.h."
  exit 1
}

Write-Host "VDD IOCTL ABI headers match."
