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
        [void] (Get-VddState $HelperScript)
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
    $script:removalCalls = 0
    $script:pollsAfterRemoval = 0
    function Get-VddDevices {
        if ($script:removalCalls -gt 0) {
            $script:pollsAfterRemoval++
            if ($script:pollsAfterRemoval -ge 3) {
                $script:fakeDevices = @()
            }
        }
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval {
        $script:removalCalls++
        return 0
    }

    [void] (Remove-AllVddDevices $HelperScript 2)
    Assert-Equal 0 $script:fakeDevices.Count 'All duplicate device nodes must be removed.'
    Assert-Equal 1 $script:removalCalls 'An asynchronous removal must not receive overlapping requests.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6')
    $script:removalCalls = 0
    function Get-VddDevices {
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval {
        $script:removalCalls++
        return 1
    }
    $removalFailedClosed = $false
    try {
        [void] (Remove-AllVddDevices $HelperScript 30)
    }
    catch {
        $removalFailedClosed = $_.Exception.Message -like '*exit code 1*'
    }
    Assert-Equal $true $removalFailedClosed 'A failed nefcon request must abort immediately.'
    Assert-Equal 1 $script:removalCalls 'A stalled removal must not repeatedly invoke nefcon.'

    $script:removalCalls = 0
    function Invoke-NefconRemoval {
        $script:removalCalls++
        return 0
    }
    $removalTimedOut = $false
    try {
        [void] (Remove-AllVddDevices $HelperScript 1)
    }
    catch {
        $removalTimedOut = $_.Exception.Message -like '*timed out*'
    }
    Assert-Equal $true $removalTimedOut 'A successful request without PnP progress must time out.'
    Assert-Equal 1 $script:removalCalls 'A timed-out removal must not repeatedly invoke nefcon.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Invoke-NefconRemoval} = $originalInvokeNefconRemoval
    Remove-Variable -Name fakeDevices -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name removalCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name pollsAfterRemoval -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name removalTimedOut -ErrorAction SilentlyContinue
}

$originalResolveVddPayload = ${function:Resolve-VddPayload}
$originalGetVddPaths = ${function:Get-VddPaths}
$originalGetVddState = ${function:Get-VddState}
$originalStageVddPayload = ${function:Stage-VddPayload}
$originalRemoveAllVddDevices = ${function:Remove-AllVddDevices}
$originalRemoveVddRegistry = ${function:Remove-VddRegistry}
$originalCleanupVddPackages = ${function:Cleanup-VddPackages}
$originalSetVddConfiguration = ${function:Set-VddConfiguration}
$originalInstallVddDevice = ${function:Install-VddDevice}
try {
    function Resolve-VddPayload {
        return [pscustomobject]@{
            RawBuild = '22000'
            BuildNumber = 22000
            DriverDir = $PSScriptRoot
            ConfigSource = $HelperScript
            Paths = [pscustomobject]@{ Nefcon = $HelperScript }
        }
    }
    function Get-VddPaths {
        return [pscustomobject]@{
            Nefcon = $HelperScript
            Dist = Join-Path $PSScriptRoot 'missing-test-dist'
        }
    }
    function Get-VddState {
        return [pscustomobject]@{
            BundledVersion = '100.0.16.6'
            Decision = $script:workflowDecision
        }
    }
    function Stage-VddPayload { $script:workflowCalls += 'Stage' }
    function Remove-AllVddDevices { $script:workflowCalls += 'Remove' }
    function Remove-VddRegistry { $script:workflowCalls += 'Registry' }
    function Cleanup-VddPackages([string] $ExpectedVersion = '') {
        $script:workflowCalls += "Cleanup:$ExpectedVersion"
    }
    function Set-VddConfiguration { $script:workflowCalls += 'Configure' }
    function Install-VddDevice { $script:workflowCalls += 'Install' }

    $script:workflowCalls = @()
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 0
        InstallRequired = 0
        CurrentVersion = '100.0.16.6'
        CurrentStatus = 'OK'
        CurrentInf = 'oem42.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run'
    Assert-Equal 'Stage,Cleanup:100.0.16.6,Configure' ($script:workflowCalls -join ',') `
        'A healthy matching device must keep its package and skip reinstall.'

    $script:workflowCalls = @()
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 1
        InstallRequired = 1
        CurrentVersion = '100.0.16.5'
        CurrentStatus = 'OK'
        CurrentInf = 'oem40.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run'
    Assert-Equal 'Stage,Remove,Registry,Cleanup:,Configure,Install' ($script:workflowCalls -join ',') `
        'An upgrade must remove, prune, configure, and install in order.'

    $script:workflowCalls = @()
    Invoke-VddUninstall $PSScriptRoot
    Assert-Equal 'Remove,Cleanup:,Registry' ($script:workflowCalls -join ',') `
        'Uninstall must remove devices before packages and always clean the registry.'
}
finally {
    ${function:Resolve-VddPayload} = $originalResolveVddPayload
    ${function:Get-VddPaths} = $originalGetVddPaths
    ${function:Get-VddState} = $originalGetVddState
    ${function:Stage-VddPayload} = $originalStageVddPayload
    ${function:Remove-AllVddDevices} = $originalRemoveAllVddDevices
    ${function:Remove-VddRegistry} = $originalRemoveVddRegistry
    ${function:Cleanup-VddPackages} = $originalCleanupVddPackages
    ${function:Set-VddConfiguration} = $originalSetVddConfiguration
    ${function:Install-VddDevice} = $originalInstallVddDevice
    Remove-Variable -Name workflowCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name workflowDecision -Scope Script -ErrorAction SilentlyContinue
}

$results | Format-Table -AutoSize
Write-Host 'VDD helper smoke tests passed.'
