[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall')]
    [string] $Action,

    [ValidateSet('Run', 'ProbeOnly', 'ResolveOnly')]
    [string] $Mode = 'Run',

    [string] $ScriptDirectory
)

$ErrorActionPreference = 'Stop'
if (-not $ScriptDirectory) {
    $ScriptDirectory = $PSScriptRoot
}
$hardwareId = 'Root\ZakoVDD'
$displayClassGuid = '4d36e968-e325-11ce-bfc1-08002be10318'
$serviceName = 'ZAKO_HDR_FOR_SUNSHINE'
$deviceEnumPath = 'SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
$deviceClassPath = 'SYSTEM\CurrentControlSet\Control\Class'
$dnStarted = 0x00000008
$crInvalidDevnode = 0x00000005
$crNoSuchDevnode = 0x0000000D
$deviceTimeoutSeconds = 120

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
        return 'MISSING'
    }
    if ($locateResult -ne 0) {
        return 'ERROR'
    }

    $statusResult = [SunshineVddCfgMgr32]::CM_Get_DevNode_Status(
        [ref] $status,
        [ref] $problem,
        $deviceInstance,
        0)
    if ($statusResult -in @($crInvalidDevnode, $crNoSuchDevnode)) {
        return 'MISSING'
    }
    if ($statusResult -eq 0 -and $problem -eq 0 -and ($status -band $dnStarted)) {
        return 'OK'
    }
    return 'ERROR'
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
                if (-not ($hardwareIds | Where-Object { $_ -ieq $hardwareId })) {
                    continue
                }

                $instanceId = "ROOT\DISPLAY\$instanceName"
                $deviceStatus = Get-VddDeviceStatus $instanceId
                if ($deviceStatus -eq 'MISSING') {
                    continue
                }

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
                    Version = $driverVersion
                    InfName = $publishedInf
                    Status = $deviceStatus
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

function Get-VddDecision([object[]] $Devices, [string] $BundledVersion) {
    $deviceList = @($Devices)
    $device = if ($deviceList.Count -eq 1) { $deviceList[0] } else { $null }
    $isReady = $device -and
        $device.Status -eq 'OK' -and
        $device.Version -eq $BundledVersion -and
        $device.InfName -match '(?i)^oem\d+\.inf$'

    return [pscustomobject]@{
        DeviceCount = $deviceList.Count
        CleanupRequired = [int]($deviceList.Count -gt 0 -and -not $isReady)
        InstallRequired = [int](-not $isReady)
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
    return [pscustomobject]@{
        BundledVersion = $bundledVersion
        Decision = Get-VddDecision $devices $bundledVersion
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

function Invoke-NefconRemoval([string] $Path) {
    & $Path `
        --remove-device-node `
        --hardware-id $hardwareId `
        --class-guid $displayClassGuid
    return $LASTEXITCODE
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
    $exitCode = Invoke-NefconRemoval $NefconPath
    if ($exitCode -notin @(0, 3010)) {
        throw "nefcon failed to remove VDD device nodes (exit code $exitCode)."
    }

    # Nefcon removes every matching node in one call, but Windows can finish
    # the PnP removal asynchronously after nefcon returns.
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    do {
        Start-Sleep -Milliseconds 250
        $devices = @(Get-VddDevices)
        if ($devices.Count -eq 0) {
            return
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw ("VDD device removal timed out. Remaining instances: " +
        ($devices.InstanceId -join ', '))
}

function Get-PublishedVddPackages([string[]] $RequiredInfNames = @()) {
    # Match the exact hardware ID instead of parsing localized pnputil output.
    $infDirectory = Join-Path $env:SystemRoot 'INF'
    $packages = foreach ($inf in @(Get-ChildItem -LiteralPath $infDirectory -Filter 'oem*.inf' -File)) {
        $matched = $false
        try {
            $matched = [bool] (Select-String `
                -LiteralPath $inf.FullName `
                -SimpleMatch $hardwareId `
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
            $devices[0].Status -ne 'OK' -or
            $devices[0].Version -ne $ExpectedVersion -or
            $devices[0].InfName -notmatch '(?i)^oem\d+\.inf$') {
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
    if (-not (Test-Path -LiteralPath (Join-Path $destination 'ZakoVDD.inf'))) {
        throw "Failed to stage the VDD payload in $destination"
    }
}

function Remove-VddRegistry([switch] $All) {
    $subKey = if ($All) { 'SOFTWARE\ZakoTech' } else { 'SOFTWARE\ZakoTech\ZakoDisplayAdapter' }
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($subKey, $false)
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

function Install-VddDevice([object] $Payload, [string] $ExpectedVersion) {
    $certificate = Join-Path $Payload.Paths.Dist 'ZakoVDD.cer'
    $inf = Join-Path $Payload.Paths.Dist 'ZakoVDD.inf'
    $nefcon = $Payload.Paths.Nefcon
    foreach ($requiredPath in @($certificate, $inf, $nefcon)) {
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

    Write-Output 'Creating VDD adapter...'
    & $nefcon `
        --create-device-node `
        --hardware-id $hardwareId `
        --service-name $serviceName `
        --class-name Display `
        --class-guid $displayClassGuid
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create the VDD device node (exit code $LASTEXITCODE)."
    }

    & $nefcon --install-driver --inf-path $inf
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to bind the VDD driver (exit code $LASTEXITCODE)."
    }

    if (-not (Wait-Until {
        $devices = @(Get-VddDevices)
        $devices.Count -eq 1 -and
            $devices[0].Status -eq 'OK' -and
            $devices[0].Version -eq $ExpectedVersion -and
            $devices[0].InfName -match '(?i)^oem\d+\.inf$'
    } $deviceTimeoutSeconds)) {
        throw "VDD did not become ready at version $ExpectedVersion."
    }
}

function Invoke-VddInstall([string] $Directory, [string] $InstallMode) {
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
    $decision = $state.Decision
    Write-Output "Existing VDD devices: $($decision.DeviceCount)"
    if ($decision.CurrentVersion) { Write-Output "Installed VDD version: $($decision.CurrentVersion)" }
    if ($decision.CurrentStatus) { Write-Output "Installed VDD status: $($decision.CurrentStatus)" }
    if ($decision.CurrentInf) { Write-Output "Active VDD package: $($decision.CurrentInf)" }
    Write-Output "Bundled VDD version: $($state.BundledVersion)"
    if ($decision.InstallRequired) {
        Write-Output 'VDD state requires reconciliation before installation.'
    }
    else {
        Write-Output 'Exactly one matching VDD device is active; skipping driver reinstall.'
    }
    if ($InstallMode -eq 'ProbeOnly') {
        return
    }

    Stage-VddPayload $payload
    if ($decision.CleanupRequired) {
        Remove-AllVddDevices $payload.Paths.Nefcon
        Remove-VddRegistry
    }

    if ($decision.InstallRequired) {
        Write-Output 'Removing superseded VDD driver packages before installation...'
        Cleanup-VddPackages
    }
    else {
        Write-Output 'Pruning stale VDD driver packages...'
        Cleanup-VddPackages $state.BundledVersion
    }

    Set-VddConfiguration $payload
    if ($decision.InstallRequired) {
        Install-VddDevice $payload $state.BundledVersion
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
    Remove-VddRegistry -All
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
        'Install' { Invoke-VddInstall $ScriptDirectory $Mode }
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
