[CmdletBinding()]
param(
    [ValidateSet('Probe', 'Ready', 'Remove', 'CleanupPackages')]
    [string] $Action,

    [string] $InfPath,
    [string] $ExpectedVersion,
    [string] $NefconPath,
    [switch] $KeepActivePackage,

    [ValidateRange(1, 60)]
    [int] $TimeoutSeconds = 10
)

$ErrorActionPreference = 'Stop'
$hardwareId = 'Root\ZakoVDD'
$displayClassGuid = '4d36e968-e325-11ce-bfc1-08002be10318'
$deviceEnumPath = 'SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY'
$deviceClassPath = 'SYSTEM\CurrentControlSet\Control\Class'
$dnStarted = 0x00000008

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
    if ($locateResult -ne 0) {
        return 'ERROR'
    }

    $statusResult = [SunshineVddCfgMgr32]::CM_Get_DevNode_Status(
        [ref] $status,
        [ref] $problem,
        $deviceInstance,
        0)
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

                $instanceId = "ROOT\DISPLAY\$instanceName"
                [pscustomobject]@{
                    InstanceId = $instanceId
                    Version = $driverVersion
                    InfName = $publishedInf
                    Status = Get-VddDeviceStatus $instanceId
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

function Wait-Until([scriptblock] $Condition) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Invoke-NefconRemoval {
    & $NefconPath `
        --remove-device-node `
        --hardware-id $hardwareId `
        --class-guid $displayClassGuid
    return $LASTEXITCODE
}

function Remove-AllVddDevices {
    $devices = @(Get-VddDevices)
    if ($devices.Count -eq 0) {
        return
    }
    if (-not $NefconPath -or -not (Test-Path -LiteralPath $NefconPath)) {
        throw "nefcon is unavailable: $NefconPath"
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $stalledPasses = 0
    do {
        $beforeCount = $devices.Count
        Write-Output "Removing VDD device node ($beforeCount remaining)..."
        [void] (Invoke-NefconRemoval)
        Start-Sleep -Milliseconds 250
        $devices = @(Get-VddDevices)

        if ($devices.Count -eq 0) {
            return
        }
        if ($devices.Count -lt $beforeCount) {
            $stalledPasses = 0
        }
        else {
            $stalledPasses++
            if ($stalledPasses -ge 8) {
                break
            }
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw ("VDD device removal made no progress. Remaining instances: " +
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

function Cleanup-VddPackages {
    $devices = @(Get-VddDevices)
    $keepInfName = ''

    if ($KeepActivePackage) {
        if (-not $ExpectedVersion) {
            throw 'ExpectedVersion is required when keeping the active VDD package.'
        }
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

function Invoke-VddDeviceHelper {
    switch ($Action) {
        'Probe' {
            $devices = @(Get-VddDevices)
            $bundledVersion = Get-BundledVersion $InfPath
            $decision = Get-VddDecision $devices $bundledVersion

            Write-Output 'VDD_PROBE_OK=1'
            Write-Output ('VDD_DEVICE_COUNT=' + $decision.DeviceCount)
            Write-Output ('VDD_CLEANUP_REQUIRED=' + $decision.CleanupRequired)
            Write-Output ('VDD_INSTALL_REQUIRED=' + $decision.InstallRequired)
            Write-Output ('CURRENT_VDD_VERSION=' + $decision.CurrentVersion)
            Write-Output ('CURRENT_VDD_STATUS=' + $decision.CurrentStatus)
            Write-Output ('CURRENT_VDD_INF=' + $decision.CurrentInf)
            Write-Output ('BUNDLED_VDD_VERSION=' + $bundledVersion)
        }
        'Ready' {
            if (-not (Wait-Until {
                $devices = @(Get-VddDevices)
                $devices.Count -eq 1 -and
                    $devices[0].Status -eq 'OK' -and
                    $devices[0].Version -eq $ExpectedVersion -and
                    $devices[0].InfName -match '(?i)^oem\d+\.inf$'
            })) {
                throw "VDD did not become ready at version $ExpectedVersion."
            }
        }
        'Remove' {
            Remove-AllVddDevices
        }
        'CleanupPackages' {
            Cleanup-VddPackages
        }
        default {
            throw 'Action is required.'
        }
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
