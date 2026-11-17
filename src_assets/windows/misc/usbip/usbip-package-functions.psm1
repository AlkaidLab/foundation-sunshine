#requires -version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:RecordKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AlkaidLabUSBip'
$script:HardwareId = 'ROOT\USBIP_WIN2\UDE'
$script:DetachTask = 'USBip Detach All On Reboot Or Shutdown'
$script:NeedsRestart = $false

function Invoke-UsbipCommand([string]$File, [string[]]$Arguments) {
  & $File @Arguments | Out-Host
  $rc = $LASTEXITCODE
  if ($rc -eq 3010) { $script:NeedsRestart = $true }
  elseif ($rc -ne 0) { throw "$([IO.Path]::GetFileName($File)) failed: $rc" }
}

function Read-UsbipManifest([string]$Root) {
  $manifest = Get-Content -LiteralPath (Join-Path $Root 'payload.json') -Raw | ConvertFrom-Json
  if ($manifest.schema -ne 1 -or $manifest.layout -ne 'usbip-slim-v1' -or $manifest.version -ne '0.9.8.1') { throw 'Unsupported USB/IP payload' }
  $required = @('usbip.exe', 'devnode.exe', 'libusbip.dll', 'resources.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'driver/usbip2_ude.inf', 'driver/usbip2_ude.sys', 'driver/usbip2_ude.cat',
    'driver/usbip2_filter.inf', 'driver/usbip2_filter.sys', 'driver/usbip2_filter.cat')
  $names = @($manifest.files.PSObject.Properties.Name)
  if (@(Compare-Object $required $names).Count -ne 0) { throw 'Incomplete or unexpected USB/IP file list' }
  foreach ($file in $manifest.files.PSObject.Properties) {
    $path = Join-Path $Root $file.Name
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.Value.sha256) { throw "USB/IP file hash mismatch: $($file.Name)" }
  }
  if (-not (Test-Path -LiteralPath (Join-Path $Root 'LICENSE'))) { throw 'USB/IP license is missing' }
  return $manifest
}

function Get-UsbipLegacyEntries {
  # Fixed upstream AppId from usbip-slim-setup-20261117, GetInstalledUninstallString.
  foreach ($base in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
    $key = Join-Path $base '{199505b0-b93d-4521-a8c7-897818e0205a}_is1'
    if (Test-Path -LiteralPath $key) { Get-ItemProperty -LiteralPath $key }
  }
}

function Remove-UsbipLegacy {
  foreach ($entry in @(Get-UsbipLegacyEntries)) {
    if ($entry.UninstallString -notmatch '^"([^"]+\\unins[0-9]+\.exe)"$') { throw 'Unrecognized upstream USB/IP uninstaller; remove it manually' }
    $uninstaller = $Matches[1]
    $process = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    if ($process.ExitCode -notin 0, 3010) { throw "Upstream USB/IP uninstall failed: $($process.ExitCode)" }
    $script:NeedsRestart = $true
  }
  if (@(Get-UsbipLegacyEntries).Count -ne 0) { throw 'Upstream USB/IP registration remains; replacement was not attempted' }
}

function Get-UsbipOemPackages {
  # usbip-slim-dism-20261117, -Online / -All: structured output, no localized
  # pnputil text parsing and no writes to the DriverDatabase registry.
  Get-WindowsDriver -Online -All -ErrorAction Stop | Where-Object {
    [IO.Path]::GetFileName($_.OriginalFileName) -in 'usbip2_ude.inf', 'usbip2_filter.inf' -and $_.Driver -match '^oem[0-9]+\.inf$'
  }
}

function Set-UsbipDetachTask([string]$Root, [switch]$Remove) {
  # usbip-slim-setup-20261117, RegisterDetachTask; Task Scheduler 2.0.
  $scheduler = New-Object -ComObject 'Schedule.Service'
  $scheduler.Connect()
  $folder = $scheduler.GetFolder('\')
  if ($Remove) {
    try { $null = $folder.GetTask($script:DetachTask) }
    catch {
      if (($_.Exception.HResult -band 0xffff) -eq 2) { return }
      throw
    }
    $folder.DeleteTask($script:DetachTask, 0)
    return
  }
  $task = $scheduler.NewTask(0)
  $task.Principal.UserId = 'SYSTEM'
  $task.Principal.LogonType = 5
  $task.Principal.RunLevel = 1
  $task.Settings.Enabled = $true
  $action = $task.Actions.Create(0)
  $action.Path = Join-Path $Root 'usbip.exe'
  $action.Arguments = 'detach --all=closeonly'
  $null = $folder.RegisterTaskDefinition($script:DetachTask, $task, 6, '', '', 5)
}

function Remove-UsbipPackage([string]$Root, [switch]$KeepFiles) {
  $failures = @()
  # Each cleanup stage runs even if an earlier one failed; preserve recovery
  # tools and the receipt until all OS removal steps have succeeded.
  try { Set-UsbipDetachTask $Root -Remove } catch { $failures += $_ }
  try {
    if ((Test-Path -LiteralPath (Join-Path $Root 'devnode.exe')) -and @(Get-UsbipNodes -IncludeAbsent).Count -gt 0) {
      Invoke-UsbipCommand (Join-Path $Root 'devnode.exe') @('remove', $script:HardwareId, 'root')
    }
  } catch { $failures += $_ }
  try {
    foreach ($package in @(Get-UsbipOemPackages)) {
      try { Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/delete-driver', $package.Driver, '/uninstall', '/force') }
      catch { $failures += $_ }
    }
  } catch { $failures += $_ }
  if ($failures.Count -ne 0) { throw "USB/IP cleanup incomplete ($($failures.Count) failed steps); recovery files retained" }
  if (Test-Path -LiteralPath $script:RecordKey) { Remove-Item -LiteralPath $script:RecordKey -Recurse -Force }
  if (-not $KeepFiles) { Remove-UsbipFiles $Root }
}

function Remove-UsbipFiles([string]$Root) {
  # Never remove unrelated files or a legacy installation recursively.
  if (Test-Path -LiteralPath (Join-Path $Root 'payload.json')) {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $Root 'payload.json') | ConvertFrom-Json
    foreach ($name in @($manifest.files.PSObject.Properties.Name) + @('payload.json', 'LICENSE', 'usbip-package.ps1', 'usbip-package-functions.psm1', 'usbip-roothub-guard.ps1')) {
      if ($name -notmatch '^(driver/)?[A-Za-z0-9_.-]+$') { throw 'Invalid receipt member' }
      $path = Join-Path $Root $name
      if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    $driver = Join-Path $Root 'driver'
    if ((Test-Path $driver) -and @(Get-ChildItem -Force $driver).Count -eq 0) { Remove-Item -LiteralPath $driver }
    if (@(Get-ChildItem -Force $Root).Count -eq 0) { Remove-Item -LiteralPath $Root }
  }
}

function Get-UsbipNodes([switch]$IncludeAbsent) {
  # usbip-slim-ms-pnp-property-20261117: hardware IDs are device properties,
  # not instance IDs. Upstream creates ROOT\USB instances; later repairs may
  # use a different enumerator while retaining the exact hardware ID.
  foreach ($device in @(Get-PnpDevice -PresentOnly:(-not $IncludeAbsent) -ErrorAction Stop | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
    $ids = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction Stop).Data
    if ($ids -contains $script:HardwareId) { $device }
  }
}

# usbip-slim-ms-pnp-property-20261117: typed HardwareIds and ProblemCode;
# usbip-slim-ms-pnp-device-20261117: PresentOnly, independent of root enumerator name.
function Test-UsbipReady([string]$Root, $Manifest) {
  try {
    $null = Read-UsbipManifest $Root
    $nodes = @(Get-UsbipNodes)
    if ($nodes.Count -ne 1) { return $false }
    $problem = (Get-PnpDeviceProperty -InstanceId $nodes[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction Stop).Data
    if ($problem -notin 0, 14) { return $false }
    foreach ($name in 'usbip2_ude', 'usbip2_filter') {
      $service = Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Services\' + $name) -ErrorAction Stop
      $image = [string]$service.ImagePath
      $image = $image -replace '^\\SystemRoot\\', ($env:SystemRoot + '\') -replace '^\\\?\?\\', ''
      if (-not [IO.Path]::IsPathRooted($image)) { $image = Join-Path $env:SystemRoot $image }
      $expected = $Manifest.files.PSObject.Properties['driver/' + $name + '.sys'].Value.sha256
      if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash -ne $expected) { return $false }
    }
    return $true
  } catch { return $false }
}

function Install-UsbipPackage([string]$Payload, [string]$Root, [string]$Scripts) {
  $manifest = Read-UsbipManifest $Payload
  Remove-UsbipLegacy
  # A partial prior slim install must be removed before replacing its tools.
  if ((Test-Path -LiteralPath $script:RecordKey) -or (Test-Path -LiteralPath (Join-Path $Root 'payload.json'))) { Remove-UsbipPackage $Root }
  New-Item -ItemType Directory -Path $Root -Force | Out-Null
  foreach ($name in @($manifest.files.PSObject.Properties.Name) + @('payload.json', 'LICENSE')) {
    $destination = Join-Path $Root $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Payload $name) -Destination $destination -Force
  }
  foreach ($name in 'usbip-package.ps1', 'usbip-package-functions.psm1', 'usbip-roothub-guard.ps1') {
    Copy-Item -LiteralPath (Join-Path $Scripts $name) -Destination (Join-Path $Root $name) -Force
  }
  $null = Read-UsbipManifest $Root
  # usbip-slim-pnputil-20261117 /add-driver. Never edit an INF or register
  # services manually: the signed INF owns AddService/AddReg/AddFilter.
  Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/add-driver', (Join-Path $Root 'driver\usbip2_ude.inf'))
  Invoke-UsbipCommand "$env:SystemRoot\System32\pnputil.exe" @('/add-driver', (Join-Path $Root 'driver\usbip2_filter.inf'), '/install')
  Invoke-UsbipCommand (Join-Path $Root 'devnode.exe') @('install', (Join-Path $Root 'driver\usbip2_ude.inf'), $script:HardwareId)
  Set-UsbipDetachTask $Root
  if (-not (Test-UsbipReady $Root $manifest)) { throw 'USB/IP controller or registered driver did not verify' }
  New-Item -Path $script:RecordKey -Force | Out-Null
  foreach ($entry in @{
    DisplayName = "USBip $($manifest.version)"; DisplayVersion = $manifest.version; Publisher = 'usbip-win2';
    InstallLocation = $Root; InstallLayout = $manifest.layout; SourceSHA256 = $manifest.installer_sha256;
    UninstallString = ('"{0}\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "{1}\usbip-package.ps1" -Action uninstall' -f $env:SystemRoot, $Root)
  }.GetEnumerator()) {
    New-ItemProperty -LiteralPath $script:RecordKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
  }
  New-ItemProperty -LiteralPath $script:RecordKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
  New-ItemProperty -LiteralPath $script:RecordKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
}

function Get-UsbipRestartCode { if ($script:NeedsRestart) { return 3010 }; return 0 }
Export-ModuleMember -Function *-Usbip*
