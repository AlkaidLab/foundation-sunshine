[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Probe', 'Ready', 'Removed')]
    [string] $Action,

    [string] $InfPath,
    [string] $ExpectedVersion,

    [ValidateRange(1, 60)]
    [int] $TimeoutSeconds = 10
)

$ErrorActionPreference = 'Stop'
$hardwareId = 'Root\ZakoVDD'

function Get-VddDevice {
    foreach ($candidate in @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)) {
        if (@($candidate.HardwareID) -contains $hardwareId) {
            return $candidate
        }
    }
    return $null
}

function Get-InstalledVersion([Microsoft.Management.Infrastructure.CimInstance] $Device) {
    if (-not $Device) {
        return ''
    }
    return (Get-PnpDeviceProperty `
        -InstanceId $Device.InstanceId `
        -KeyName DEVPKEY_Device_DriverVersion `
        -ErrorAction SilentlyContinue).Data
}

function Get-BundledVersion([string] $Path) {
    $driverVer = (Select-String `
        -LiteralPath $Path `
        -Pattern '^\s*DriverVer\s*=' `
        -ErrorAction Stop).Line
    if (-not $driverVer) {
        throw "DriverVer was not found in $Path"
    }
    return ($driverVer -split ',')[-1].Trim()
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

try {
    switch ($Action) {
        'Probe' {
            $device = Get-VddDevice
            $bundledVersion = Get-BundledVersion $InfPath
            Write-Output 'VDD_PROBE_OK=1'
            Write-Output ('VDD_DEVICE_PRESENT=' + [int]($null -ne $device))
            Write-Output ('CURRENT_VDD_VERSION=' + (Get-InstalledVersion $device))
            Write-Output ('CURRENT_VDD_STATUS=' + $(if ($device) { $device.Status } else { '' }))
            Write-Output ('BUNDLED_VDD_VERSION=' + $bundledVersion)
            exit 0
        }
        'Removed' {
            if (Wait-Until { $null -eq (Get-VddDevice) }) {
                exit 0
            }
            exit 1
        }
        'Ready' {
            if (Wait-Until {
                $device = Get-VddDevice
                $device -and
                    $device.Status -eq 'OK' -and
                    (Get-InstalledVersion $device) -eq $ExpectedVersion
            }) {
                exit 0
            }
            exit 1
        }
    }
} catch {
    Write-Error $_
    exit 1
}
