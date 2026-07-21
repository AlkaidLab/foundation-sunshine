[CmdletBinding()]
param(
    [string] $HelperScript
)

$ErrorActionPreference = 'Stop'

if (-not $HelperScript) {
    $HelperScript = Join-Path $PSScriptRoot 'vdd-device-helper.ps1'
}

function Assert-Equal($Expected, $Actual, [string] $Message) {
    if ($Expected -ne $Actual) {
        throw "$Message`nExpected: $Expected`nActual:   $Actual"
    }
}

function New-TestDevice(
    [string] $Version,
    [string] $Status = 'OK',
    [string] $InfName = 'oem42.inf') {
    return [pscustomobject]@{
        InstanceId = 'ROOT\DISPLAY\0042'
        Version = $Version
        Status = $Status
        InfName = $InfName
        Problem = $(if ($Status -eq 'OK') { 0 } else { 10 })
    }
}

if (-not (Test-Path -LiteralPath $HelperScript)) {
    throw "VDD helper not found: $HelperScript"
}

. $HelperScript

$cases = @(
    @{
        Name = 'No device requires one install'
        Devices = @()
        Count = 0
        Cleanup = 0
        Install = 1
    },
    @{
        Name = 'One healthy matching device is a no-op'
        Devices = @(New-TestDevice '100.0.16.6')
        Count = 1
        Cleanup = 0
        Install = 0
    },
    @{
        Name = 'Version upgrade reconciles the existing device'
        Devices = @(New-TestDevice '100.0.16.5')
        Count = 1
        Cleanup = 1
        Install = 1
    },
    @{
        Name = 'Unhealthy device is replaced'
        Devices = @(New-TestDevice '100.0.16.6' 'ERROR')
        Count = 1
        Cleanup = 1
        Install = 1
    },
    @{
        Name = 'Device without a published INF is reconciled'
        Devices = @(New-TestDevice '100.0.16.6' 'OK' '')
        Count = 1
        Cleanup = 1
        Install = 1
    },
    @{
        Name = 'Duplicate devices are collapsed to one'
        Devices = @(
            (New-TestDevice '100.0.16.6' 'OK' 'oem42.inf'),
            (New-TestDevice '100.0.16.6' 'OK' 'oem42.inf')
        )
        Count = 2
        Cleanup = 1
        Install = 1
    }
)

$results = foreach ($case in $cases) {
    $decision = Get-VddDecision $case.Devices '100.0.16.6'
    Assert-Equal $case.Count $decision.DeviceCount "$($case.Name): wrong device count"
    Assert-Equal $case.Cleanup $decision.CleanupRequired "$($case.Name): wrong cleanup decision"
    Assert-Equal $case.Install $decision.InstallRequired "$($case.Name): wrong install decision"

    [pscustomobject]@{
        Case = $case.Name
        Count = $decision.DeviceCount
        Cleanup = $decision.CleanupRequired
        Install = $decision.InstallRequired
        Status = 'PASS'
    }
}

$packages = @(
    [pscustomobject]@{ InfName = 'oem40.inf' },
    [pscustomobject]@{ InfName = 'OEM42.INF' },
    [pscustomobject]@{ InfName = 'oem43.inf' }
)
$stalePackages = @(Get-VddPackagesToRemove $packages 'oem42.inf')
Assert-Equal 2 $stalePackages.Count 'Package pruning must keep exactly the active OEM INF.'
Assert-Equal 'oem40.inf,oem43.inf' ($stalePackages.InfName -join ',') 'Wrong stale package selection.'

$invalidKeepWasRejected = $false
try {
    [void] (Get-VddPackagesToRemove $packages 'ZakoVDD.inf')
}
catch {
    $invalidKeepWasRejected = $true
}
Assert-Equal $true $invalidKeepWasRejected 'Package pruning must reject a non-published INF name.'

$originalGetChildItem = ${function:Get-ChildItem}
$originalSelectString = ${function:Select-String}
try {
    $script:fakeInfFiles = @(
        [pscustomobject]@{ Name = 'oem40.inf'; FullName = 'C:\fake\oem40.inf' },
        [pscustomobject]@{ Name = 'oem42.inf'; FullName = 'C:\fake\oem42.inf' },
        [pscustomobject]@{ Name = 'oem43.inf'; FullName = 'C:\fake\oem43.inf' }
    )
    function Get-ChildItem {
        [CmdletBinding()]
        param([string] $LiteralPath, [string] $Filter, [switch] $File)
        return @($script:fakeInfFiles)
    }
    function Select-String {
        [CmdletBinding()]
        param(
            [string] $LiteralPath,
            [string] $SimpleMatch,
            [switch] $Quiet
        )
        if ($LiteralPath -like '*oem40.inf') {
            throw 'simulated unreadable INF'
        }
        return $LiteralPath -like '*oem42.inf'
    }

    $foundPackages = @(Get-PublishedVddPackages `
        -RequiredInfNames @('oem42.inf'))
    Assert-Equal 1 $foundPackages.Count 'An unreadable unrelated INF must not abort the package scan.'
    Assert-Equal 'oem42.inf' $foundPackages[0].InfName 'The readable VDD package must still be detected.'

    $activeReadFailedClosed = $false
    try {
        [void] (Get-PublishedVddPackages -RequiredInfNames @('oem40.inf'))
    }
    catch {
        $activeReadFailedClosed = $_.Exception.Message -like '*active VDD package oem40.inf*'
    }
    Assert-Equal $true $activeReadFailedClosed 'An unreadable active VDD package must abort the scan.'
}
finally {
    if ($originalGetChildItem) {
        ${function:Get-ChildItem} = $originalGetChildItem
    }
    else {
        Remove-Item Function:\Get-ChildItem -ErrorAction SilentlyContinue
    }
    if ($originalSelectString) {
        ${function:Select-String} = $originalSelectString
    }
    else {
        Remove-Item Function:\Select-String -ErrorAction SilentlyContinue
    }
    Remove-Variable -Name fakeInfFiles -Scope Script -ErrorAction SilentlyContinue
}

$originalGetVddDevices = ${function:Get-VddDevices}
try {
    function Get-VddDevices {
        throw 'simulated device enumeration failure'
    }

    $probeFailedClosed = $false
    try {
        $Action = 'Probe'
        $InfPath = $HelperScript
        [void] (Invoke-VddDeviceHelper)
    }
    catch {
        $probeFailedClosed = $_.Exception.Message -like '*simulated device enumeration failure*'
    }
    Assert-Equal $true $probeFailedClosed 'A device enumeration failure must abort the probe.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
}

$originalGetVddDevices = ${function:Get-VddDevices}
$originalInvokeNefconRemoval = ${function:Invoke-NefconRemoval}
try {
    $firstFakeDevice = New-TestDevice '100.0.16.5' 'OK' 'oem40.inf'
    $secondFakeDevice = New-TestDevice '100.0.16.6' 'OK' 'oem42.inf'
    $secondFakeDevice.InstanceId = 'ROOT\DISPLAY\0043'
    $script:fakeDevices = @($firstFakeDevice, $secondFakeDevice)
    function Get-VddDevices {
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval {
        $script:fakeDevices = @($script:fakeDevices | Select-Object -Skip 1)
        return 0
    }

    $NefconPath = $HelperScript
    $TimeoutSeconds = 2
    [void] (Remove-AllVddDevices)
    Assert-Equal 0 $script:fakeDevices.Count 'All duplicate device nodes must be removed.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6')
    function Invoke-NefconRemoval {
        return 1
    }
    $removalFailedClosed = $false
    try {
        [void] (Remove-AllVddDevices)
    }
    catch {
        $removalFailedClosed = $_.Exception.Message -like '*made no progress*'
    }
    Assert-Equal $true $removalFailedClosed 'A failed removal must abort before creating another node.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Invoke-NefconRemoval} = $originalInvokeNefconRemoval
    Remove-Variable -Name fakeDevices -Scope Script -ErrorAction SilentlyContinue
}

$results | Format-Table -AutoSize
Write-Host 'VDD helper smoke tests passed.'
