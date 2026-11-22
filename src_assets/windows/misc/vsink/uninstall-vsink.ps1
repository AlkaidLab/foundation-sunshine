#requires -version 5.1
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$script:restartSuggested = $false

function Test-VbCableDevice($Device) {
  return $Device.Class -eq 'MEDIA' -and
    $Device.Manufacturer -ieq 'VB-Audio Software' -and
    @($Device.HardwareID) -icontains 'VBAudioVACWDM'
}

function Get-CurrentDriverBindings {
  # Win32_PnPSignedDriver may keep reporting removed bindings. Read the PnP
  # properties of all devices, including those currently not present, so a
  # package shared with another device is never removed accidentally.
  # PnP nodes can disappear while the list is being read. Discard an
  # incomplete snapshot and retry rather than treating it as a safe list.
  for ($attempt = 0; $attempt -lt 3; $attempt++) {
    try {
      $bindings = [System.Collections.Generic.List[object]]::new()
      foreach ($device in @(Get-PnpDevice -ErrorAction Stop)) {
        if (-not $device.InstanceId) {
          throw 'Could not identify all devices; refusing to remove a driver package.'
        }
        try {
          $property = @(Get-PnpDeviceProperty -InputObject $device -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction Stop)
        } catch {
          throw "Could not read driver binding for $($device.InstanceId): $($_.Exception.Message)"
        }
        if ($property.Count -ne 1 -or $property[0].InstanceId -ine $device.InstanceId) {
          throw 'Could not verify a device driver binding; refusing to remove a driver package.'
        }
        $inf = [string]$property[0].Data
        $isVbCable = Test-VbCableDevice $device
        if (-not $inf -and $isVbCable -and $device.Present) {
          throw 'The VB-Cable device has no verifiable driver package.'
        }
        if ($inf) {
          $bindings.Add([pscustomobject]@{
            IsVbCable = $isVbCable
            Present = [bool]$device.Present
            InstanceId = [string]$device.InstanceId
            InfName = $inf
          })
        }
      }
      return $bindings.ToArray()
    } catch {
      if ($attempt -eq 2) { throw "Could not inspect device bindings: $($_.Exception.Message)" }
      Start-Sleep -Milliseconds 500
    }
  }
}

function Get-VbCablePackages([object[]]$Bindings, [object[]]$InstalledPackages) {
  $vbDevices = @($Bindings | Where-Object { $_.IsVbCable })
  foreach ($device in $vbDevices) {
    if ([string]$device.InfName -notmatch '^oem[0-9]+\.inf$') {
      throw 'The VB-Cable driver has no valid published OEM INF name.'
    }
  }
  $boundPackages = @($vbDevices | ForEach-Object { [string]$_.InfName })
  $packages = @($InstalledPackages | Where-Object {
    $original = [IO.Path]::GetFileName([string]$_.OriginalFileName)
    $_.ProviderName -ieq 'VB-Audio Software' -and $_.ClassName -ieq 'MEDIA' -and
      -not $_.Inbox -and [string]$_.Driver -match '^oem[0-9]+\.inf$' -and
      ($boundPackages -icontains [string]$_.Driver -or
        $original -match '^vbmmecable(?:64)?_(?:win[0-9]+|vista|xp|2003)\.inf$')
  } | ForEach-Object { ([string]$_.Driver).ToLowerInvariant() } | Select-Object -Unique)

  foreach ($device in @($vbDevices | Where-Object { $_.Present })) {
    if ($packages -inotcontains [string]$device.InfName) {
      throw 'Could not verify the installed VB-Cable driver package and publisher.'
    }
  }
  foreach ($package in $packages) {
    if (@($Bindings | Where-Object { $_.InfName -ieq $package -and -not $_.IsVbCable }).Count) {
      throw "Driver package $package is also used by another device; use the VB-Audio uninstaller instead."
    }
  }
  return $packages
}

function Test-VbCableServiceStopped {
  $service = @(Get-CimInstance -ClassName Win32_SystemDriver -Filter "Name='VBAudioVACMME'" -ErrorAction Stop)
  if ($service.Count -eq 0) { return $true }
  if ($service.Count -ne 1) { throw 'Could not verify that the VB-Cable driver has stopped.' }
  return $service[0].State -eq 'Stopped'
}

function Get-ServiceDeviceReferences([string]$ServiceName) {
  foreach ($device in @(Get-PnpDevice -ErrorAction Stop)) {
    if (-not $device.InstanceId) { throw 'Could not identify all devices.' }
    $property = @(Get-PnpDeviceProperty -InputObject $device -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop)
    if ($property.Count -ne 1 -or $property[0].InstanceId -ine $device.InstanceId) {
      throw 'Could not verify all device service bindings.'
    }
    if ([string]$property[0].Data -ieq $ServiceName) { $device.InstanceId }
  }
}

function Remove-VbCableOrphanService([object[]]$InstalledPackages, [scriptblock]$ReadService, [scriptblock]$ReadReferences, [scriptblock]$DeleteService, [scriptblock]$WaitForRefresh) {
  $services = @(& $ReadService)
  if ($services.Count -eq 0) { return }
  if ($services.Count -ne 1 -or $services[0].Name -ine 'VBAudioVACMME' -or $services[0].ServiceType -ne 'Kernel Driver') {
    throw 'Could not verify the residual VB-Cable driver service.'
  }
  $service = $services[0]
  if ($service.State -ne 'Stopped') { throw 'VBCABLE-DEVICE-BUSY: VB-Cable is still in use or being released.' }

  # Only clean the known VB-Cable kernel service, and only when no VB-Audio
  # media package or device refers to it. Do not stop a service or delete files.
  if (@($InstalledPackages | Where-Object { $_.ProviderName -ieq 'VB-Audio Software' -and $_.ClassName -ieq 'MEDIA' }).Count) {
    throw 'Another VB-Audio media driver remains; the residual service was not removed.'
  }
  $image = [Environment]::ExpandEnvironmentVariables(([string]$service.PathName).Trim().Trim('"'))
  if ($image -match '^\\SystemRoot\\') { $image = Join-Path $env:SystemRoot $image.Substring(12) }
  if ($image -match '^\\\?\?\\') { $image = $image.Substring(4) }
  try {
    $image = [IO.Path]::GetFullPath($image)
    $parent = [IO.Path]::GetDirectoryName($image)
    $drivers = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\drivers')).TrimEnd('\')
    $store = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository')).TrimEnd('\')
  } catch { throw 'The residual VB-Cable service has an invalid driver path.' }
  $isDriverStore = $parent.StartsWith($store + '\', [StringComparison]::OrdinalIgnoreCase) -and
    [IO.Path]::GetFileName($parent) -match '^vbmmecable(?:64)?_[a-z0-9]+\.inf_'
  if ((-not $parent.Equals($drivers, [StringComparison]::OrdinalIgnoreCase) -and -not $isDriverStore) -or
      [IO.Path]::GetFileName($image) -notmatch '^vbaudio_cable(?:64)?_(?:win[0-9]+|vista|xp|2003)\.sys$') {
    throw 'The residual service does not point to a recognised VB-Cable driver.'
  }
  if (@(& $ReadReferences).Count) { throw 'A device still references the VB-Cable driver service.' }
  $code = & $DeleteService
  if ($code -eq 1072) { $script:restartSuggested = $true; return }
  if ($code -ne 0) { throw "The residual VB-Cable service could not be removed (exit $code)." }
  for ($retry = 0; $retry -lt 10; $retry++) {
    if (@(& $ReadService).Count -eq 0) { return }
    & $WaitForRefresh
  }
  # Windows may keep a deleted service until other handles close.
  $script:restartSuggested = $true
}

function Remove-VbCableDrivers([scriptblock]$ReadBindings, [scriptblock]$ReadPackages, [scriptblock]$RemoveDevice, [scriptblock]$RemovePackage, [scriptblock]$IsDriverStopped, [scriptblock]$WaitForRefresh, [scriptblock]$RemoveOrphanService) {
  $packagesPendingRestart = @()
  # Refresh both the device tree and the driver store after every change: an
  # older installed package can become bound while a device is being removed.
  for ($pass = 0; $pass -lt 32; $pass++) {
    $bindings = @(& $ReadBindings)
    try { $installedPackages = @(& $ReadPackages) }
    catch { throw "Could not inspect the driver store: $($_.Exception.Message)" }
    $packages = @(Get-VbCablePackages $bindings $installedPackages)
    $devices = @($bindings | Where-Object { $_.IsVbCable -and $_.Present })
    if ($devices.Count) {
      if ($packagesPendingRestart.Count) { return }
      $devicePendingRestart = $false
      foreach ($device in $devices) {
        if ([string]$device.InstanceId -notmatch '^ROOT\\[A-Za-z0-9_.&{}-]+\\[A-Za-z0-9_.&{}-]+$') {
          throw 'The VB-Cable device has an unexpected instance ID; refusing to remove it.'
        }
      }
      foreach ($device in $devices) {
        Write-Host "Removing VB-Cable device $($device.InstanceId)..."
        $code = & $RemoveDevice $device.InstanceId
        if ($code -eq 3010) {
          $script:restartSuggested = $true
          $devicePendingRestart = $true
        }
        elseif ($code -in @(32, 170, 2404)) { throw 'VBCABLE-DEVICE-BUSY: Windows reports that the device is in use.' }
        elseif ($code -ne 0) { throw "VB-Cable device $($device.InstanceId) could not be removed (exit $code)." }
      }
      # Re-read after device removal; success from pnputil does not by itself
      # prove that the device has gone or that the kernel driver has stopped.
      $detached = $false
      for ($retry = 0; $retry -le 10; $retry++) {
        $current = @(& $ReadBindings)
        if (@($current | Where-Object { $_.IsVbCable -and $_.Present }).Count -eq 0 -and (& $IsDriverStopped)) {
          $detached = $true
          break
        }
        if ($retry -lt 10) { & $WaitForRefresh }
      }
      if (-not $detached) {
        if ($devicePendingRestart) { return }
        throw 'VBCABLE-DEVICE-BUSY: VB-Cable is still in use or being released.'
      }
      continue
    }

    # A residual package can outlive the device. Wait only for the service
    # here, then take a fresh binding snapshot before touching any package.
    if (-not (& $IsDriverStopped)) {
      $stopped = $false
      for ($retry = 0; $retry -lt 10; $retry++) {
        & $WaitForRefresh
        if (& $IsDriverStopped) { $stopped = $true; break }
      }
      if (-not $stopped) {
        if ($packagesPendingRestart.Count) { return }
        throw 'VBCABLE-DEVICE-BUSY: VB-Cable is still in use or being released.'
      }
      continue
    }
    if ($packages.Count -eq 0) {
      & $RemoveOrphanService $installedPackages
      return
    }

    # This package was validated against the current driver store and every
    # device binding above. pnputil is deliberately invoked without /force.
    # A 3010 package may stay listed until reboot. Leave it pending while
    # processing other verified packages, and keep its service while it remains.
    $package = $packages | Where-Object { $_ -notin $packagesPendingRestart } | Select-Object -First 1
    if (-not $package) { return }
    Write-Host "Removing VB-Cable driver package $package..."
    $code = & $RemovePackage $package
    if ($code -eq 3010) {
      $script:restartSuggested = $true
      $packagesPendingRestart += $package
      continue
    }
    if ($code -in @(32, 170, 2404)) { throw 'VBCABLE-DEVICE-BUSY: Windows reports that the driver package is in use.' }
    if ($code -ne 0) { throw "Driver package $package could not be removed (exit $code)." }
  }
  throw 'Could not verify VB-Cable driver removal.'
}

try {
  Remove-VbCableDrivers `
    { Get-CurrentDriverBindings } `
    { Get-WindowsDriver -Online -ErrorAction Stop } `
    { param($instance) & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /remove-device $instance | Out-Host; $LASTEXITCODE } `
    { param($package) & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /delete-driver $package | Out-Host; $LASTEXITCODE } `
    { Test-VbCableServiceStopped } `
    { Start-Sleep -Milliseconds 500 } `
    { param($packages) Remove-VbCableOrphanService $packages `
        { Get-CimInstance -ClassName Win32_SystemDriver -Filter "Name='VBAudioVACMME'" -ErrorAction Stop } `
        { Get-ServiceDeviceReferences 'VBAudioVACMME' } `
        { & (Join-Path $env:SystemRoot 'System32\sc.exe') delete VBAudioVACMME | Out-Host; $LASTEXITCODE } `
        { Start-Sleep -Milliseconds 500 } }
  if ($script:restartSuggested) { exit 3010 }
  exit 0
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
  if ($script:restartSuggested) { [Console]::Error.WriteLine('Windows also reported that a restart may be needed.') }
  exit 1
}
