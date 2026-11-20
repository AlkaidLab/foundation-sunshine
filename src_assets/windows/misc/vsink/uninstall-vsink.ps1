#requires -version 5.1
$ErrorActionPreference = 'Stop'

function Get-VbCablePackages([object[]]$Drivers) {
  $devices = @($Drivers | Where-Object {
    $_.DeviceName -ieq 'VB-Audio Virtual Cable' -and $_.DriverProviderName -ieq 'VB-Audio Software'
  })
  if ($devices.Count -eq 0) {
    throw 'The VB-Cable driver could not be identified. Use the VB-Audio uninstaller instead.'
  }

  foreach ($device in $devices) {
    if ([string]$device.InfName -notmatch '^oem[0-9]+\.inf$') {
      throw 'The VB-Cable driver has no valid published OEM INF name.'
    }
  }
  $packages = @($devices | ForEach-Object { ([string]$_.InfName).ToLowerInvariant() } | Select-Object -Unique)
  foreach ($package in $packages) {
    $otherDevices = @($Drivers | Where-Object {
      $_.InfName -ieq $package -and
        ($_.DeviceName -ine 'VB-Audio Virtual Cable' -or $_.DriverProviderName -ine 'VB-Audio Software')
    })
    if ($otherDevices.Count -ne 0) {
      throw "Driver package $package is also used by another device; use the VB-Audio uninstaller instead."
    }
  }
  return $packages
}

try {
  $drivers = @(Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop)
  $packages = @(Get-VbCablePackages $drivers)
  $restart = $false
  foreach ($package in $packages) {
    Write-Host "Removing VB-Cable driver package $package..."
    & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /delete-driver $package /uninstall
    if ($LASTEXITCODE -eq 3010) { $restart = $true }
    elseif ($LASTEXITCODE -ne 0) { throw "Driver package $package could not be removed (exit $LASTEXITCODE)." }
  }
  if ($restart) { exit 3010 }
  exit 0
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
  exit 1
}
