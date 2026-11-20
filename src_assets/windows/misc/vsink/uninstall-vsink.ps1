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

function Remove-VbCableDrivers([scriptblock]$ReadDrivers, [scriptblock]$RemovePackage, [scriptblock]$WaitForRefresh) {
  $attempted = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  $unchanged = 0
  for ($pass = 0; $pass -le 16; $pass++) {
    $drivers = @(& $ReadDrivers)
    $devices = @($drivers | Where-Object {
      $_.DeviceName -ieq 'VB-Audio Virtual Cable' -and $_.DriverProviderName -ieq 'VB-Audio Software'
    })
    if ($devices.Count -eq 0) {
      if ($attempted.Count -eq 0) { throw 'The VB-Cable driver could not be identified. Use the VB-Audio uninstaller instead.' }
      return
    }

    # Windows can bind the device to an older installed package after removal.
    # Check ownership and sharing again before touching any newly bound INF.
    $packages = @(Get-VbCablePackages $drivers)
    $newPackages = @($packages | Where-Object { -not $attempted.Contains($_) })
    if ($newPackages.Count -eq 0) {
      if ($script:RestartRequired) {
        Write-Host 'A restart is required before VB-Cable removal can be verified.'
        return
      }
      if ($unchanged -ge 2) { throw 'VB-Cable is still present after driver removal. Use the VB-Audio uninstaller instead.' }
      $unchanged++
      & $WaitForRefresh
      continue
    }
    if ($pass -eq 16) { throw 'Too many VB-Cable driver packages; use the VB-Audio uninstaller instead.' }
    $unchanged = 0
    # Re-enumerate before removing the next package so sharing checks never
    # rely on the device bindings from before a previous removal.
    $package = $newPackages[0]
    [void]$attempted.Add($package)
    Write-Host "Removing VB-Cable driver package $package..."
    $code = & $RemovePackage $package
    if ($code -eq 3010) { $script:RestartRequired = $true }
    elseif ($code -ne 0) { throw "Driver package $package could not be removed (exit $code)." }
  }
  throw 'Could not verify VB-Cable driver removal.'
}

$script:RestartRequired = $false
try {
  Remove-VbCableDrivers `
    { Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop } `
    { param($package) & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /delete-driver $package /uninstall | Out-Host; $LASTEXITCODE } `
    { Start-Sleep -Milliseconds 500 }
  if ($script:RestartRequired) { exit 3010 }
  exit 0
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
  if ($script:RestartRequired) { [Console]::Error.WriteLine('A restart is also required to finish an earlier driver removal.') }
  exit 1
}
