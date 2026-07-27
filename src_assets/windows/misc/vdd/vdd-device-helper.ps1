[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')]
    [string] $Action,

    [ValidateSet('Run', 'ProbeOnly', 'ResolveOnly')]
    [string] $Mode = 'Run',

    [string] $ScriptDirectory,

    [switch] $PreserveHealthyExisting
)

$ErrorActionPreference = 'Stop'
if (-not $ScriptDirectory) {
    $ScriptDirectory = $PSScriptRoot
}
$hardwareId = 'Root\ZakoVDD'
$managedHardwareIds = @($hardwareId, 'ZakoVDD')
$displayClassGuid = '4d36e968-e325-11ce-bfc1-08002be10318'
$serviceName = 'ZAKO_HDR_FOR_SUNSHINE'
$deviceEnumPath = 'SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
$deviceClassPath = 'SYSTEM\CurrentControlSet\Control\Class'
$dnStarted = 0x00000008
$crInvalidDevnode = 0x00000005
$crNoSuchDevnode = 0x0000000D
$crBufferSmall = 0x0000001A
$deviceTimeoutSeconds = 120
$targetedRemovalTimeoutSeconds = 10
$win32ErrorTimeout = 1460
$vddControlInterfaceGuid = 'DA9F8C2B-7E4F-49A1-9D4E-6F2B0E1A0C4D'
$requiredDriverFiles = @('ZakoVDD.inf', 'ZakoVDD.dll', 'zakovdd.cat', 'ZakoVDD.cer')

function Initialize-CfgMgr32 {
    if ('SunshineVddCfgMgr32' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SunshineVddCfgMgr32
{
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Locate_DevNodeW(
        out uint deviceInstance,
        string deviceInstanceId,
        uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern uint CM_Get_DevNode_Status(
        out uint status,
        out uint problemNumber,
        uint deviceInstance,
        uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Get_Device_Interface_List_SizeW(
        out uint bufferLength,
        ref Guid interfaceClassGuid,
        string deviceInstanceId,
        uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern uint CM_Get_Device_Interface_ListW(
        ref Guid interfaceClassGuid,
        string deviceInstanceId,
        [Out] char[] buffer,
        uint bufferLength,
        uint flags);
}
'@
}

function Get-VddDeviceStatus([string] $InstanceId) {
    Initialize-CfgMgr32

    [uint32] $deviceInstance = 0
    [uint32] $status = 0
    [uint32] $problem = 0
    $locateResult = [SunshineVddCfgMgr32]::CM_Locate_DevNodeW(
        [ref] $deviceInstance,
        $InstanceId,
        0)
    if ($locateResult -in @($crInvalidDevnode, $crNoSuchDevnode)) {
        return [pscustomobject]@{ State = 'MISSING'; Problem = 0 }
    }
    if ($locateResult -ne 0) {
        return [pscustomobject]@{ State = 'ERROR'; Problem = 0 }
    }

    $statusResult = [SunshineVddCfgMgr32]::CM_Get_DevNode_Status(
        [ref] $status,
        [ref] $problem,
        $deviceInstance,
        0)
    if ($statusResult -in @($crInvalidDevnode, $crNoSuchDevnode)) {
        return [pscustomobject]@{ State = 'MISSING'; Problem = 0 }
    }
    if ($statusResult -eq 0 -and $problem -eq 0 -and ($status -band $dnStarted)) {
        return [pscustomobject]@{ State = 'OK'; Problem = [int]$problem }
    }
    return [pscustomobject]@{ State = 'ERROR'; Problem = [int]$problem }
}

function Test-VddControlInterfaceAvailable(
    [int] $Attempts = 3,
    [int] $DelayMilliseconds = 200) {
    Initialize-CfgMgr32

    $interfaceGuid = [Guid] $vddControlInterfaceGuid
    for ($attempt = 0; $attempt -lt [Math]::Max(1, $Attempts); $attempt++) {
        [uint32] $bufferLength = 0
        $sizeResult = [SunshineVddCfgMgr32]::CM_Get_Device_Interface_List_SizeW(
            [ref] $bufferLength,
            [ref] $interfaceGuid,
            $null,
            0)
        if ($sizeResult -eq 0 -and $bufferLength -gt 1) {
            $buffer = New-Object char[] $bufferLength
            $listResult = [SunshineVddCfgMgr32]::CM_Get_Device_Interface_ListW(
                [ref] $interfaceGuid,
                $null,
                $buffer,
                $bufferLength,
                0)
            if ($listResult -eq 0 -and $buffer[0] -ne [char]0) {
                return $true
            }
            if ($listResult -eq $crBufferSmall) {
                continue
            }
        }

        if ($attempt + 1 -lt $Attempts -and $DelayMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $DelayMilliseconds
        }
    }
    return $false
}

function Get-VddDevices {
    # Get-PnpDevice uses CIM and can fail with 0x800705af under memory pressure.
    # The registry plus cfgmgr32 gives us locale-independent, fail-closed state.
    $registryView = if ([Environment]::Is64BitOperatingSystem) {
        [Microsoft.Win32.RegistryView]::Registry64
    }
    else {
        [Microsoft.Win32.RegistryView]::Default
    }
    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        $registryView)
    $enumKey = $null

    try {
        $enumKey = $baseKey.OpenSubKey($deviceEnumPath)
        if (-not $enumKey) {
            return @()
        }

        $devices = foreach ($instanceName in $enumKey.GetSubKeyNames()) {
            $deviceKey = $null
            $driverKey = $null
            try {
                $deviceKey = $enumKey.OpenSubKey($instanceName)
                if (-not $deviceKey) {
                    continue
                }

                $hardwareIds = @($deviceKey.GetValue('HardwareID', $null))
                $matchedHardwareId = @($hardwareIds | Where-Object {
                    $managedHardwareIds -icontains $_
                } | Select-Object -First 1)
                if ($matchedHardwareId.Count -eq 0) {
                    continue
                }

                $instanceId = "ROOT\DISPLAY\$instanceName"
                $deviceStatus = Get-VddDeviceStatus $instanceId

                $driverVersion = ''
                $publishedInf = ''
                $driverKeyName = [string] $deviceKey.GetValue('Driver', '')
                if ($driverKeyName) {
                    $driverKey = $baseKey.OpenSubKey("$deviceClassPath\$driverKeyName")
                    if ($driverKey) {
                        $driverVersion = [string] $driverKey.GetValue('DriverVersion', '')
                        $publishedInf = [string] $driverKey.GetValue('InfPath', '')
                    }
                }

                [pscustomobject]@{
                    InstanceId = $instanceId
                    HardwareId = [string] $matchedHardwareId[0]
                    Version = $driverVersion
                    InfName = $publishedInf
                    Status = $deviceStatus.State
                    Problem = $deviceStatus.Problem
                }
            }
            finally {
                if ($driverKey) {
                    $driverKey.Dispose()
                }
                if ($deviceKey) {
                    $deviceKey.Dispose()
                }
            }
        }
        return @($devices)
    }
    finally {
        if ($enumKey) {
            $enumKey.Dispose()
        }
        $baseKey.Dispose()
    }
}

function Get-BundledVersion([string] $Path) {
    if (-not $Path) {
        throw 'InfPath is required when probing the bundled VDD version.'
    }

    $driverVer = (Select-String `
        -LiteralPath $Path `
        -Pattern '^\s*DriverVer\s*=' `
        -ErrorAction Stop).Line
    if (-not $driverVer) {
        throw "DriverVer was not found in $Path"
    }
    return ($driverVer -split ',')[-1].Trim()
}

function Test-VddDeviceReady([object] $Device, [string] $ExpectedVersion = '') {
    return [bool]($Device -and
        $Device.Status -eq 'OK' -and
        $Device.InfName -match '(?i)^oem\d+\.inf$' -and
        (-not $ExpectedVersion -or $Device.Version -eq $ExpectedVersion))
}

function Test-VddVersionAtLeast([string] $InstalledVersion, [string] $BundledVersion) {
    try {
        return [version] $InstalledVersion -ge [version] $BundledVersion
    }
    catch {
        # Never silently replace a healthy custom driver whose version cannot
        # be compared with the bundled Windows dotted-quad version.
        return $true
    }
}

function Get-VddDecision(
    [object[]] $Devices,
    [string] $BundledVersion,
    [bool] $ControlAvailable = $true) {
    $deviceList = @($Devices)
    $device = if ($deviceList.Count -eq 1) { $deviceList[0] } else { $null }
    $isHealthy = Test-VddDeviceReady $device
    $isReady = (Test-VddDeviceReady $device $BundledVersion) -and $ControlAvailable

    return [pscustomobject]@{
        DeviceCount = $deviceList.Count
        CleanupRequired = [int]($deviceList.Count -gt 0 -and -not $isReady)
        InstallRequired = [int](-not $isReady)
        HealthyExisting = [int]$isHealthy
        ControlAvailable = [int]$ControlAvailable
        CurrentVersion = $(if ($device) { $device.Version } else { '' })
        CurrentStatus = $(if ($device) { $device.Status } else { '' })
        CurrentInf = $(if ($device) { $device.InfName } else { '' })
    }
}

function Get-VddPaths([string] $Directory) {
    if (-not $Directory) {
        throw 'ScriptDirectory is required.'
    }

    $scripts = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $root = [IO.Path]::GetFullPath((Join-Path $scripts '..'))
    $dist = Join-Path $root 'tools\vdd'
    $nefcon = Join-Path $root 'tools\nefconw.exe'
    if (-not (Test-Path -LiteralPath $nefcon)) {
        $nefcon = Join-Path $dist 'nefconw.exe'
    }

    return [pscustomobject]@{
        Scripts = $scripts
        Root = $root
        DriverRoot = Join-Path $scripts 'driver'
        Dist = $dist
        ConfigDir = Join-Path $root 'config'
        Nefcon = $nefcon
    }
}

function Resolve-VddPayload([string] $Directory, [string] $BuildOverride) {
    $paths = Get-VddPaths $Directory
    $rawBuild = $BuildOverride
    if (-not $rawBuild) {
        $registryView = if ([Environment]::Is64BitOperatingSystem) {
            [Microsoft.Win32.RegistryView]::Registry64
        }
        else {
            [Microsoft.Win32.RegistryView]::Default
        }
        $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine,
            $registryView)
        $versionKey = $baseKey.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion')
        try {
            if ($versionKey) {
                $rawBuild = [string] $versionKey.GetValue('CurrentBuildNumber', '')
                if (-not $rawBuild) {
                    $rawBuild = [string] $versionKey.GetValue('CurrentBuild', '')
                }
            }
        }
        finally {
            if ($versionKey) {
                $versionKey.Dispose()
            }
            $baseKey.Dispose()
        }
    }

    [int] $buildNumber = 0
    $hasBuildNumber = [int]::TryParse($rawBuild, [ref] $buildNumber)
    if (-not $rawBuild) {
        Write-Warning 'Could not detect Windows build; defaulting to Win10 payload.'
    }
    elseif (-not $hasBuildNumber) {
        Write-Warning "Ignoring non-numeric Windows build '$rawBuild'; defaulting to Win10 payload."
    }

    $latest = Join-Path $paths.DriverRoot 'latest'
    $driverDir = Join-Path $paths.DriverRoot 'win10'
    if (-not (Test-Path -LiteralPath (Join-Path $driverDir 'ZakoVDD.inf'))) {
        $driverDir = $latest
    }
    if ($hasBuildNumber -and $buildNumber -ge 22000 -and
        (Test-Path -LiteralPath (Join-Path $latest 'ZakoVDD.inf'))) {
        $driverDir = $latest
    }
    if (-not (Test-Path -LiteralPath (Join-Path $driverDir 'ZakoVDD.inf'))) {
        $driverDir = $paths.DriverRoot
    }

    $configSource = Join-Path $paths.DriverRoot 'vdd_settings.xml'
    if (Test-Path -LiteralPath (Join-Path $driverDir 'vdd_settings.xml')) {
        $configSource = Join-Path $driverDir 'vdd_settings.xml'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $driverDir 'ZakoVDD.inf'))) {
        throw "VDD driver payload not found in $driverDir"
    }
    if (-not (Test-Path -LiteralPath $configSource)) {
        throw "VDD configuration template not found at $configSource"
    }

    return [pscustomobject]@{
        Paths = $paths
        RawBuild = $rawBuild
        BuildNumber = $(if ($hasBuildNumber) { $buildNumber } else { $null })
        DriverDir = $driverDir
        ConfigSource = $configSource
    }
}

function Get-VddState([string] $InfPath) {
    $devices = @(Get-VddDevices)
    $bundledVersion = Get-BundledVersion $InfPath
    $controlAvailable = Test-VddControlInterfaceAvailable
    return [pscustomobject]@{
        Devices = $devices
        BundledVersion = $bundledVersion
        ControlAvailable = $controlAvailable
        Decision = Get-VddDecision $devices $bundledVersion $controlAvailable
    }
}

function Wait-Until([scriptblock] $Condition, [int] $WaitSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    do {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId = $hardwareId) {
    & $Path `
        --remove-device-node `
        --hardware-id $TargetHardwareId `
        --class-guid $displayClassGuid
    return $LASTEXITCODE
}

function Invoke-PnpUtilDeviceRemoval([string] $InstanceId) {
    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    $process = Start-Process `
        -FilePath $pnputil `
        -ArgumentList @('/remove-device', $InstanceId) `
        -NoNewWindow `
        -PassThru
    try {
        Wait-Process `
            -InputObject $process `
            -Timeout $targetedRemovalTimeoutSeconds `
            -ErrorAction Stop
    }
    catch {
        if ($process.HasExited) {
            return $process.ExitCode
        }
        Stop-Process -InputObject $process -Force -ErrorAction SilentlyContinue
        return $win32ErrorTimeout
    }
    return $process.ExitCode
}

function Remove-AllVddDevices([string] $NefconPath, [int] $WaitSeconds = $deviceTimeoutSeconds) {
    $devices = @(Get-VddDevices)
    if ($devices.Count -eq 0) {
        return
    }
    if (-not $NefconPath -or -not (Test-Path -LiteralPath $NefconPath)) {
        throw "nefcon is unavailable: $NefconPath"
    }

    Write-Output "Removing VDD device nodes ($($devices.Count) found)..."
    $targetHardwareIds = @($devices | ForEach-Object {
        if ($_.HardwareId) { $_.HardwareId } else { $hardwareId }
    } | Sort-Object -Unique)
    foreach ($targetHardwareId in $targetHardwareIds) {
        $exitCode = Invoke-NefconRemoval $NefconPath $targetHardwareId
        if ($exitCode -notin @(0, 3010)) {
            throw "nefcon failed to remove VDD device nodes for $targetHardwareId (exit code $exitCode)."
        }
    }

    # Nefcon removes every matching node in one call, but Windows can finish
    # the PnP removal asynchronously after nefcon returns. Only disconnected
    # nodes need a targeted fallback; issuing a second request for live nodes can
    # race the asynchronous nefcon removal.
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    $targetedAttempts = @{}
    do {
        Start-Sleep -Milliseconds 250
        $devices = @(Get-VddDevices)
        if ($devices.Count -eq 0) {
            return
        }

        foreach ($device in $devices) {
            if ($device.Status -ne 'MISSING' -or
                $targetedAttempts.ContainsKey($device.InstanceId)) {
                continue
            }
            Write-Output "Removing remaining VDD instance $($device.InstanceId) ($($device.Status))..."
            $targetedDeadline = [DateTime]::UtcNow.AddSeconds($targetedRemovalTimeoutSeconds)
            if ($targetedDeadline -gt $deadline) {
                $targetedDeadline = $deadline
            }
            $targetedAttempts[$device.InstanceId] = [pscustomobject]@{
                ExitCode = Invoke-PnpUtilDeviceRemoval $device.InstanceId
                Deadline = $targetedDeadline
            }
        }

        $stalledTargetedDevices = @($devices | Where-Object {
            $attempt = $targetedAttempts[$_.InstanceId]
            $_.Status -eq 'MISSING' -and
                $null -ne $attempt -and
                [DateTime]::UtcNow -ge $attempt.Deadline
        })
        if ($stalledTargetedDevices.Count -gt 0) {
            $details = @($stalledTargetedDevices | ForEach-Object {
                $targetedExitCode = $targetedAttempts[$_.InstanceId].ExitCode
                "$($_.InstanceId)[$($_.HardwareId),$($_.Status),pnputil=$targetedExitCode]"
            }) -join ', '
            throw "VDD device removal made no progress. Remaining instances: $details"
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw ("VDD device removal timed out. Remaining instances: " +
        (@($devices | ForEach-Object {
            "$($_.InstanceId)[$($_.HardwareId),$($_.Status)]"
        }) -join ', '))
}

function Get-PublishedVddPackages([string[]] $RequiredInfNames = @()) {
    # Match the exact hardware ID instead of parsing localized pnputil output.
    $infDirectory = Join-Path $env:SystemRoot 'INF'
    $packages = foreach ($inf in @(Get-ChildItem -LiteralPath $infDirectory -Filter 'oem*.inf' -File)) {
        $matched = $false
        try {
            $matched = [bool] (Select-String `
                -LiteralPath $inf.FullName `
                -Pattern $managedHardwareIds `
                -SimpleMatch `
                -Quiet `
                -ErrorAction Stop)
        }
        catch {
            if ($RequiredInfNames -icontains $inf.Name) {
                throw "Failed to inspect active VDD package $($inf.Name): $($_.Exception.Message)"
            }
            Write-Warning "Skipping unreadable unrelated INF $($inf.Name): $($_.Exception.Message)"
            continue
        }

        if ($matched) {
            [pscustomobject]@{
                InfName = $inf.Name
                Path = $inf.FullName
            }
        }
    }
    return @($packages)
}

function Get-VddPackagesToRemove([object[]] $Packages, [string] $KeepInfName = '') {
    if ($KeepInfName -and $KeepInfName -notmatch '(?i)^oem\d+\.inf$') {
        throw "Invalid active OEM INF name: $KeepInfName"
    }
    return @($Packages | Where-Object { -not $KeepInfName -or $_.InfName -ine $KeepInfName })
}

function Remove-VddPackages([object[]] $Packages) {
    $pnputil = Join-Path $env:SystemRoot 'System32\pnputil.exe'
    foreach ($package in @($Packages)) {
        Write-Output "Removing VDD driver package $($package.InfName)..."
        & $pnputil /delete-driver $package.InfName /force
        if ($LASTEXITCODE -notin @(0, 3010)) {
            throw "Failed to remove VDD package $($package.InfName) (exit code $LASTEXITCODE)."
        }
    }
}

function Cleanup-VddPackages([string] $ExpectedVersion = '') {
    $devices = @(Get-VddDevices)
    $keepInfName = ''

    if ($ExpectedVersion) {
        if ($devices.Count -ne 1 -or
            -not (Test-VddDeviceReady $devices[0] $ExpectedVersion)) {
            throw 'Refusing to keep a package without exactly one ready VDD device at the expected version.'
        }
        $keepInfName = $devices[0].InfName
    }
    elseif ($devices.Count -ne 0) {
        throw "Refusing to purge VDD packages while $($devices.Count) device node(s) remain."
    }

    $requiredInfNames = if ($keepInfName) { @($keepInfName) } else { @() }
    $packages = @(Get-PublishedVddPackages -RequiredInfNames $requiredInfNames)
    Remove-VddPackages @(Get-VddPackagesToRemove $packages $keepInfName)

    $remaining = @(Get-PublishedVddPackages -RequiredInfNames $requiredInfNames)
    if ($keepInfName) {
        if ($remaining.Count -ne 1 -or $remaining[0].InfName -ine $keepInfName) {
            throw "Expected only active VDD package $keepInfName; found: $($remaining.InfName -join ', ')"
        }
        Write-Output "Active VDD driver package: $keepInfName"
    }
    elseif ($remaining.Count -ne 0) {
        throw "VDD packages remain after cleanup: $($remaining.InfName -join ', ')"
    }
    Write-Output ('VDD_DRIVER_PACKAGE_COUNT=' + $remaining.Count)
}

function Stage-VddPayload([object] $Payload) {
    foreach ($fileName in $requiredDriverFiles) {
        $sourcePath = Join-Path $Payload.DriverDir $fileName
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "Required VDD payload file is unavailable: $sourcePath"
        }
    }
    if (-not (Test-Path -LiteralPath $Payload.Paths.Nefcon)) {
        throw "Required VDD installer file is unavailable: $($Payload.Paths.Nefcon)"
    }

    $destination = $Payload.Paths.Dist
    if (Test-Path -LiteralPath $destination) {
        $preservedNefcon = [IO.Path]::GetFullPath($Payload.Paths.Nefcon)
        foreach ($item in @(Get-ChildItem -LiteralPath $destination -Force)) {
            if (-not [string]::Equals(
                $item.FullName,
                $preservedNefcon,
                [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $item.FullName -Recurse -Force
            }
        }
    }
    [void] [IO.Directory]::CreateDirectory($destination)
    foreach ($file in @(Get-ChildItem -LiteralPath $Payload.DriverDir -File)) {
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
    foreach ($fileName in $requiredDriverFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $destination $fileName))) {
            throw "Failed to stage the VDD payload in $destination"
        }
    }
}

function Remove-VddRegistry {
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree('SOFTWARE\ZakoTech', $false)
}

function Set-VddConfiguration([object] $Payload) {
    [void] [IO.Directory]::CreateDirectory($Payload.Paths.ConfigDir)
    $target = Join-Path $Payload.Paths.ConfigDir 'vdd_settings.xml'
    if (-not (Test-Path -LiteralPath $target)) {
        Copy-Item -LiteralPath $Payload.ConfigSource -Destination $target
    }
    if (-not (Test-Path -LiteralPath $target)) {
        throw "Failed to create VDD configuration file $target"
    }

    $key = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey(
        'SOFTWARE\ZakoTech\ZakoDisplayAdapter')
    try {
        $key.SetValue(
            'VDDPATH',
            $Payload.Paths.ConfigDir,
            [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally {
        $key.Dispose()
    }
}

function Add-VddDriverPackage([object] $Payload) {
    $certificate = Join-Path $Payload.Paths.Dist 'ZakoVDD.cer'
    $inf = Join-Path $Payload.Paths.Dist 'ZakoVDD.inf'
    foreach ($requiredPath in @($certificate, $inf)) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Required VDD installer file is unavailable: $requiredPath"
        }
    }

    & (Join-Path $env:SystemRoot 'System32\certutil.exe') -addstore -f root $certificate
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to import the VDD certificate (exit code $LASTEXITCODE)."
    }

    Write-Output 'Staging VDD driver package...'
    & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /add-driver $inf
    $pnputilExitCode = $LASTEXITCODE
    if ($pnputilExitCode -notin @(0, 3010)) {
        throw "Failed to stage the VDD driver package (exit code $pnputilExitCode)."
    }
    if ($pnputilExitCode -eq 3010) {
        Write-Output 'VDD driver package staged; Windows reports that a reboot is required.'
    }
}

function Install-VddDeviceFromInf(
    [string] $NefconPath,
    [string] $InfPath,
    [string] $ExpectedVersion,
    [bool] $RequireControlInterface = $true) {
    foreach ($requiredPath in @($NefconPath, $InfPath)) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Required VDD installer file is unavailable: $requiredPath"
        }
    }

    Write-Output 'Creating VDD adapter...'
    & $NefconPath `
        --create-device-node `
        --hardware-id $hardwareId `
        --service-name $serviceName `
        --class-name Display `
        --class-guid $displayClassGuid
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create the VDD device node (exit code $LASTEXITCODE)."
    }

    & $NefconPath --install-driver --inf-path $InfPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to bind the VDD driver (exit code $LASTEXITCODE)."
    }

    if (-not (Wait-Until {
        $devices = @(Get-VddDevices)
        $devices.Count -eq 1 -and
            (Test-VddDeviceReady $devices[0] $ExpectedVersion) -and
            (-not $RequireControlInterface -or
                (Test-VddControlInterfaceAvailable 1 0))
    } $deviceTimeoutSeconds)) {
        $readiness = if ($RequireControlInterface) { ' with its control interface' } else { '' }
        throw "VDD did not become ready$readiness at version $ExpectedVersion."
    }
}

function Install-VddDevice([object] $Payload, [string] $ExpectedVersion) {
    Install-VddDeviceFromInf `
        $Payload.Paths.Nefcon `
        (Join-Path $Payload.Paths.Dist 'ZakoVDD.inf') `
        $ExpectedVersion
}

function Restore-VddDevice([object] $Payload, [object] $PreviousDevice) {
    $previousInf = Join-Path (Join-Path $env:SystemRoot 'INF') $PreviousDevice.InfName
    if (-not (Test-Path -LiteralPath $previousInf)) {
        throw "Previous VDD package is unavailable: $previousInf"
    }

    $devices = @(Get-VddDevices)
    $previousDeviceStillReady = $devices.Count -eq 1 -and
        (Test-VddDeviceReady $devices[0] $PreviousDevice.Version) -and
        $devices[0].InfName -ieq $PreviousDevice.InfName
    if (-not $previousDeviceStillReady) {
        if ($devices.Count -gt 0) {
            Remove-AllVddDevices $Payload.Paths.Nefcon
        }
        Write-Output "Restoring previous VDD version $($PreviousDevice.Version)..."
        Install-VddDeviceFromInf `
            $Payload.Paths.Nefcon `
            $previousInf `
            $PreviousDevice.Version `
            $false
    }

    try {
        Cleanup-VddPackages $PreviousDevice.Version
    }
    catch {
        Write-Warning "Previous VDD was restored, but staged package cleanup failed: $($_.Exception.Message)"
    }
}

function Get-VddTransactionPath([object] $Payload) {
    return Join-Path $Payload.Paths.ConfigDir 'vdd-driver-update.json'
}

function Save-VddTransaction([object] $Payload, [object] $PreviousDevice) {
    [void] [IO.Directory]::CreateDirectory($Payload.Paths.ConfigDir)
    $transactionPath = Get-VddTransactionPath $Payload
    $temporaryPath = "$transactionPath.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [pscustomobject]@{
            Version = 1
            PreviousVersion = [string] $PreviousDevice.Version
            PreviousInf = [string] $PreviousDevice.InfName
        } | ConvertTo-Json | Set-Content -LiteralPath $temporaryPath -Encoding UTF8
        Move-Item -LiteralPath $temporaryPath -Destination $transactionPath -Force
    }
    finally {
        Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
    }
}

function Clear-VddTransaction([object] $Payload) {
    Remove-Item -LiteralPath (Get-VddTransactionPath $Payload) -Force -ErrorAction SilentlyContinue
}

function Recover-VddTransaction([object] $Payload, [string] $BundledVersion) {
    $transactionPath = Get-VddTransactionPath $Payload
    if (-not (Test-Path -LiteralPath $transactionPath)) {
        return
    }

    try {
        $transaction = Get-Content -LiteralPath $transactionPath -Raw | ConvertFrom-Json
    }
    catch {
        throw "Cannot read the pending VDD update transaction: $($_.Exception.Message)"
    }
    if ($transaction.Version -ne 1 -or
        -not $transaction.PreviousVersion -or
        $transaction.PreviousInf -notmatch '(?i)^oem\d+\.inf$') {
        throw 'The pending VDD update transaction is invalid; refusing to modify the driver.'
    }

    $devices = @(Get-VddDevices)
    $readyDevice = if ($devices.Count -eq 1 -and
        (Test-VddDeviceReady $devices[0])) {
        $devices[0]
    }
    if ($readyDevice) {
        if ($readyDevice.Version -in @($BundledVersion, $transaction.PreviousVersion)) {
            Clear-VddTransaction $Payload
            return
        }
        throw 'A pending VDD update exists, but a different healthy driver is active; refusing automatic recovery.'
    }

    Write-Output 'Recovering an interrupted VDD driver update...'
    Restore-VddDevice $Payload ([pscustomobject]@{
        Version = [string] $transaction.PreviousVersion
        InfName = [string] $transaction.PreviousInf
    })
    Clear-VddTransaction $Payload
}

function Invoke-VddInstall(
    [string] $Directory,
    [string] $InstallMode,
    [switch] $PreserveHealthyExisting) {
    $payload = Resolve-VddPayload $Directory $env:VDD_TEST_WIN_BUILD
    if ($null -ne $payload.BuildNumber) {
        Write-Output "Detected Windows build: $($payload.BuildNumber)"
    }
    elseif ($payload.RawBuild) {
        Write-Output "Detected Windows build (raw): $($payload.RawBuild)"
    }
    Write-Output "Using VDD payload: $($payload.DriverDir)"

    if ($InstallMode -eq 'ResolveOnly') {
        Write-Output ('RESOLVED_WIN_BUILD=' + $payload.RawBuild)
        Write-Output ('RESOLVED_WIN_BUILD_NUM=' + $(if ($null -ne $payload.BuildNumber) { $payload.BuildNumber } else { '' }))
        Write-Output ('RESOLVED_DRIVER_DIR=' + $payload.DriverDir)
        Write-Output ('RESOLVED_CONFIG_SOURCE=' + $payload.ConfigSource)
        return
    }

    $state = Get-VddState (Join-Path $payload.DriverDir 'ZakoVDD.inf')
    if ($InstallMode -eq 'Run' -and
        (Test-Path -LiteralPath (Get-VddTransactionPath $payload))) {
        Recover-VddTransaction $payload $state.BundledVersion
        $state = Get-VddState (Join-Path $payload.DriverDir 'ZakoVDD.inf')
    }
    $decision = $state.Decision
    Write-Output "Existing VDD devices: $($decision.DeviceCount)"
    if ($decision.CurrentVersion) { Write-Output "Installed VDD version: $($decision.CurrentVersion)" }
    if ($decision.CurrentStatus) { Write-Output "Installed VDD status: $($decision.CurrentStatus)" }
    if ($decision.CurrentInf) { Write-Output "Active VDD package: $($decision.CurrentInf)" }
    Write-Output "VDD control interface available: $([bool]$decision.ControlAvailable)"
    Write-Output "Bundled VDD version: $($state.BundledVersion)"
    if ($decision.InstallRequired) {
        Write-Output 'VDD state requires reconciliation before installation.'
    }
    else {
        Write-Output 'Exactly one matching VDD device is active; skipping driver reinstall.'
    }
    if ($InstallMode -eq 'ProbeOnly') {
        $devices = @($state.Devices)
        $decision = Get-VddDecision $devices $state.BundledVersion $state.ControlAvailable
        $device = if ($decision.DeviceCount -eq 1) { $devices[0] } else { $null }
        Write-Output 'VDD_PROBE_OK=1'
        Write-Output ('VDD_DEVICE_PRESENT=' + [int]($decision.DeviceCount -gt 0))
        Write-Output ('VDD_CONTROL_AVAILABLE=' + $decision.ControlAvailable)
        Write-Output ('CURRENT_VDD_VERSION=' + $decision.CurrentVersion)
        Write-Output ('CURRENT_VDD_STATUS=' + $decision.CurrentStatus)
        Write-Output ('CURRENT_VDD_PROBLEM=' + $(if ($device) { [int]$device.Problem } else { '' }))
        Write-Output ('BUNDLED_VDD_VERSION=' + $state.BundledVersion)
        return
    }

    Stage-VddPayload $payload
    Set-VddConfiguration $payload

    if ($PreserveHealthyExisting -and
        $decision.InstallRequired -and
        $decision.HealthyExisting -and
        $decision.ControlAvailable -and
        (Test-VddVersionAtLeast $decision.CurrentVersion $state.BundledVersion)) {
        Write-Output 'A healthy same-or-newer VDD is already active; deferring the bundled driver update.'
        Write-Output 'VDD_DRIVER_UPDATE_DEFERRED=1'
        return
    }

    if ($decision.InstallRequired) {
        # Validate the signed package with Windows before touching the working
        # device, and retain its OEM INF until the replacement is verified.
        Add-VddDriverPackage $payload

        $previousDevice = if ($decision.HealthyExisting) { $state.Devices[0] } else { $null }
        if ($previousDevice) {
            Save-VddTransaction $payload $previousDevice
        }
        try {
            if ($decision.CleanupRequired) {
                Remove-AllVddDevices $payload.Paths.Nefcon
            }
            Install-VddDevice $payload $state.BundledVersion
            if ($previousDevice) {
                Clear-VddTransaction $payload
            }
        }
        catch {
            $installFailure = $_
            if ($previousDevice) {
                try {
                    Restore-VddDevice $payload $previousDevice
                    Clear-VddTransaction $payload
                    Write-Warning 'The VDD update failed; the previous working driver was restored.'
                }
                catch {
                    throw "$($installFailure.Exception.Message) Rollback also failed: $($_.Exception.Message)"
                }
            }
            throw $installFailure
        }

        try {
            Write-Output 'Pruning superseded VDD driver packages...'
            Cleanup-VddPackages $state.BundledVersion
        }
        catch {
            Write-Warning "VDD is ready, but stale package cleanup failed: $($_.Exception.Message)"
        }
    }
    else {
        try {
            Write-Output 'Pruning stale VDD driver packages...'
            Cleanup-VddPackages $state.BundledVersion
        }
        catch {
            Write-Warning "The active VDD is ready, but stale package cleanup failed: $($_.Exception.Message)"
        }
    }
    Write-Output 'VDD installation completed.'
}

function Invoke-VddUninstall([string] $Directory) {
    $paths = Get-VddPaths $Directory
    $failure = ''
    try {
        Write-Output 'Removing all VDD device nodes...'
        Remove-AllVddDevices $paths.Nefcon
    }
    catch {
        $failure = $_.Exception.Message
        Write-Warning 'Failed to remove every VDD device node; driver packages will be preserved.'
    }

    if (-not $failure) {
        try {
            Write-Output 'Removing all VDD driver packages...'
            Cleanup-VddPackages
        }
        catch {
            $failure = $_.Exception.Message
            Write-Warning 'Failed to remove every VDD driver package.'
        }
    }

    Write-Output 'Cleaning VDD files and registry...'
    Clear-VddTransaction ([pscustomobject]@{ Paths = $paths })
    Remove-VddRegistry
    if (Test-Path -LiteralPath $paths.Dist) {
        Remove-Item -LiteralPath $paths.Dist -Recurse -Force
    }
    if ($failure) {
        throw $failure
    }
    Write-Output 'VDD uninstall completed.'
}

function Invoke-VddDeviceHelper {
    switch ($Action) {
        'Install' { Invoke-VddInstall $ScriptDirectory $Mode -PreserveHealthyExisting:$PreserveHealthyExisting }
        'Uninstall' { Invoke-VddUninstall $ScriptDirectory }
        default { throw 'Action is required.' }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        Invoke-VddDeviceHelper
        exit 0
    }
    catch {
        Write-Error $_
        exit 1
    }
}
