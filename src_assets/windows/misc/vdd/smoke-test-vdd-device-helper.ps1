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
    [string] $InfName = 'oem42.inf',
    [string] $HardwareId = 'Root\ZakoVDD') {
    return [pscustomobject]@{
        InstanceId = 'ROOT\DISPLAY\0042'
        HardwareId = $HardwareId
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

Assert-Equal $false (Test-VddVersionAtLeast '100.0.16.5' '100.0.16.6') `
    'An older driver version must not satisfy the bundled minimum.'
Assert-Equal $true (Test-VddVersionAtLeast '100.0.16.7' '100.0.16.6') `
    'A newer driver version must satisfy the bundled minimum.'
Assert-Equal $true (Test-VddVersionAtLeast 'custom-build' '100.0.16.6') `
    'An incomparable healthy custom driver must be preserved.'

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
        Name = 'A matching device without its control interface is reconciled'
        Devices = @(New-TestDevice '100.0.16.6')
        ControlAvailable = $false
        Count = 1
        Cleanup = 1
        Install = 1
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
    },
    @{
        Name = 'A disconnected legacy device is reconciled'
        Devices = @(New-TestDevice '100.0.16.5' 'MISSING' 'oem40.inf' 'ZakoVDD')
        Count = 1
        Cleanup = 1
        Install = 1
    }
)

$results = foreach ($case in $cases) {
    $controlAvailable = if ($case.ContainsKey('ControlAvailable')) {
        [bool] $case.ControlAvailable
    }
    else {
        $true
    }
    $decision = Get-VddDecision $case.Devices '100.0.16.6' $controlAvailable
    Assert-Equal $case.Count $decision.DeviceCount "$($case.Name): wrong device count"
    Assert-Equal $case.Cleanup $decision.CleanupRequired "$($case.Name): wrong cleanup decision"
    Assert-Equal $case.Install $decision.InstallRequired "$($case.Name): wrong install decision"
    Assert-Equal ([int]$controlAvailable) $decision.ControlAvailable `
        "$($case.Name): wrong control-interface decision"

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
            [string[]] $Pattern,
            [switch] $SimpleMatch,
            [switch] $Quiet
        )
        $script:lastInfPatterns = @($Pattern)
        if ($LiteralPath -like '*oem40.inf') {
            throw 'simulated unreadable INF'
        }
        return $LiteralPath -like '*oem42.inf'
    }

    $foundPackages = @(Get-PublishedVddPackages `
        -RequiredInfNames @('oem42.inf'))
    Assert-Equal 1 $foundPackages.Count 'An unreadable unrelated INF must not abort the package scan.'
    Assert-Equal 'oem42.inf' $foundPackages[0].InfName 'The readable VDD package must still be detected.'
    Assert-Equal 'Root\ZakoVDD,ZakoVDD' ($script:lastInfPatterns -join ',') `
        'Package discovery must recognize current and legacy hardware IDs.'

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
    Remove-Variable -Name lastInfPatterns -Scope Script -ErrorAction SilentlyContinue
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

$originalStartProcess = ${function:Start-Process}
$originalWaitProcess = ${function:Wait-Process}
$originalStopProcess = ${function:Stop-Process}
try {
    $script:fakePnpProcess = [pscustomobject]@{
        HasExited = $false
        ExitCode = 0
    }
    $script:pnpWaitTimesOut = $true
    $script:stoppedPnpProcesses = 0
    function Start-Process {
        [CmdletBinding()]
        param(
            [string] $FilePath,
            [object[]] $ArgumentList,
            [switch] $NoNewWindow,
            [switch] $PassThru)
        return $script:fakePnpProcess
    }
    function Wait-Process {
        [CmdletBinding()]
        param(
            [object] $InputObject,
            [int] $Timeout)
        if ($script:pnpWaitTimesOut) {
            throw 'simulated timeout'
        }
    }
    function Stop-Process {
        [CmdletBinding()]
        param(
            [object] $InputObject,
            [switch] $Force)
        $script:stoppedPnpProcesses++
    }

    $timedOutExitCode = Invoke-PnpUtilDeviceRemoval 'ROOT\DISPLAY\0042'
    Assert-Equal $win32ErrorTimeout $timedOutExitCode `
        'A hung pnputil process must return the Windows timeout error.'
    Assert-Equal 1 $script:stoppedPnpProcesses `
        'A hung pnputil process must be force-stopped.'

    $script:fakePnpProcess = [pscustomobject]@{
        HasExited = $true
        ExitCode = 3010
    }
    $script:pnpWaitTimesOut = $false
    $completedExitCode = Invoke-PnpUtilDeviceRemoval 'ROOT\DISPLAY\0042'
    Assert-Equal 3010 $completedExitCode `
        'A completed pnputil process must preserve its exit code.'
}
finally {
    foreach ($name in @('Start-Process', 'Wait-Process', 'Stop-Process')) {
        $originalName = 'original' + $name.Replace('-', '')
        $original = Get-Variable -Name $originalName -ValueOnly
        if ($original) {
            Set-Item -Path "Function:\$name" -Value $original
        }
        else {
            Remove-Item -Path "Function:\$name" -ErrorAction SilentlyContinue
        }
    }
    Remove-Variable -Name fakePnpProcess -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name pnpWaitTimesOut -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name stoppedPnpProcesses -Scope Script -ErrorAction SilentlyContinue
}

$originalGetVddDevices = ${function:Get-VddDevices}
$originalInvokeNefconRemoval = ${function:Invoke-NefconRemoval}
$originalInvokePnpUtilDeviceRemoval = ${function:Invoke-PnpUtilDeviceRemoval}
try {
    $firstFakeDevice = New-TestDevice '100.0.16.5' 'OK' 'oem40.inf'
    $secondFakeDevice = New-TestDevice '100.0.16.6' 'OK' 'oem42.inf'
    $secondFakeDevice.InstanceId = 'ROOT\DISPLAY\0043'
    $script:fakeDevices = @($firstFakeDevice, $secondFakeDevice)
    $script:removalCalls = 0
    $script:targetedRemovalCalls = 0
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
    function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId) {
        $script:removalCalls++
        return 0
    }
    function Invoke-PnpUtilDeviceRemoval {
        $script:targetedRemovalCalls++
        return 0
    }

    [void] (Remove-AllVddDevices $HelperScript 2)
    Assert-Equal 0 $script:fakeDevices.Count 'All duplicate device nodes must be removed.'
    Assert-Equal 1 $script:removalCalls 'An asynchronous removal must not receive overlapping requests.'
    Assert-Equal 0 $script:targetedRemovalCalls 'Live nodes must not receive overlapping targeted removal requests.'

    $legacyDevice = New-TestDevice '100.0.16.5' 'MISSING' 'oem40.inf' 'ZakoVDD'
    $legacyDevice.InstanceId = 'ROOT\DISPLAY\0043'
    $script:fakeDevices = @((New-TestDevice '100.0.16.6'), $legacyDevice)
    $script:removalCalls = 0
    $script:targetedRemovalCalls = 0
    $script:pollsAfterRemoval = 0
    $script:removedHardwareIds = @()
    function Get-VddDevices {
        if ($script:removalCalls -gt 0) {
            $script:pollsAfterRemoval++
            if ($script:pollsAfterRemoval -ge 2) {
                $script:fakeDevices = @()
            }
        }
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId) {
        $script:removalCalls++
        $script:removedHardwareIds += $TargetHardwareId
        return 0
    }

    [void] (Remove-AllVddDevices $HelperScript 2)
    Assert-Equal 2 $script:removalCalls 'Root and legacy hardware IDs must both be reconciled.'
    Assert-Equal 'Root\ZakoVDD,ZakoVDD' ($script:removedHardwareIds -join ',') `
        'Removal must target both managed hardware IDs.'
    Assert-Equal 1 $script:targetedRemovalCalls 'A disconnected legacy node must receive targeted cleanup.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6' 'MISSING')
    $script:removalCalls = 0
    function Get-VddDevices {
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId) {
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
    function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId) {
        $script:removalCalls++
        return 0
    }
    $removalMadeNoProgress = $false
    try {
        [void] (Remove-AllVddDevices $HelperScript 1)
    }
    catch {
        $removalMadeNoProgress = $_.Exception.Message -like '*made no progress*pnputil=0*'
    }
    Assert-Equal $true $removalMadeNoProgress 'A successful request without PnP progress must fail with diagnostics.'
    Assert-Equal 1 $script:removalCalls 'A stalled removal must not repeatedly invoke nefcon.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6' 'OK')
    $script:removalCalls = 0
    $script:targetedRemovalCalls = 0
    $liveRemovalTimedOut = $false
    try {
        [void] (Remove-AllVddDevices $HelperScript 1)
    }
    catch {
        $liveRemovalTimedOut = $_.Exception.Message -like '*timed out*'
    }
    Assert-Equal $true $liveRemovalTimedOut 'A live asynchronous removal must keep the normal timeout path.'
    Assert-Equal 0 $script:targetedRemovalCalls 'A live node must not receive the disconnected-node fallback.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Invoke-NefconRemoval} = $originalInvokeNefconRemoval
    ${function:Invoke-PnpUtilDeviceRemoval} = $originalInvokePnpUtilDeviceRemoval
    Remove-Variable -Name fakeDevices -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name removalCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name pollsAfterRemoval -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name removalMadeNoProgress -ErrorAction SilentlyContinue
    Remove-Variable -Name liveRemovalTimedOut -ErrorAction SilentlyContinue
    Remove-Variable -Name targetedRemovalCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name removedHardwareIds -Scope Script -ErrorAction SilentlyContinue
}

$transactionTestRoot = Join-Path ([IO.Path]::GetTempPath()) "sunshine-vdd-transaction-$([Guid]::NewGuid().ToString('N'))"
[void] [IO.Directory]::CreateDirectory($transactionTestRoot)
$transactionPayload = [pscustomobject]@{
    Paths = [pscustomobject]@{
        ConfigDir = $transactionTestRoot
        Nefcon = $HelperScript
    }
}
$transactionPreviousDevice = New-TestDevice '100.0.16.5' 'OK' 'oem40.inf'
$originalGetVddDevices = ${function:Get-VddDevices}
$originalRestoreVddDevice = ${function:Restore-VddDevice}
try {
    $script:transactionDevices = @()
    $script:transactionRestoreCalls = 0
    function Get-VddDevices { return @($script:transactionDevices) }
    function Restore-VddDevice { $script:transactionRestoreCalls++ }

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    Recover-VddTransaction $transactionPayload '100.0.16.6'
    Assert-Equal 1 $script:transactionRestoreCalls `
        'An interrupted replacement without a ready VDD must restore the previous package.'
    Assert-Equal $false (Test-Path -LiteralPath (Get-VddTransactionPath $transactionPayload)) `
        'A recovered VDD transaction must be cleared.'

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    $script:transactionDevices = @(New-TestDevice '100.0.16.6')
    Recover-VddTransaction $transactionPayload '100.0.16.6'
    Assert-Equal 1 $script:transactionRestoreCalls `
        'A completed replacement must not roll back the new ready VDD.'

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    $script:transactionDevices = @(New-TestDevice '100.0.17.0')
    $unexpectedDriverWasRejected = $false
    try {
        Recover-VddTransaction $transactionPayload '100.0.16.6'
    }
    catch {
        $unexpectedDriverWasRejected = $_.Exception.Message -like '*different healthy driver*'
    }
    Assert-Equal $true $unexpectedDriverWasRejected `
        'Recovery must not replace a different healthy VDD automatically.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Restore-VddDevice} = $originalRestoreVddDevice
    if (Test-Path -LiteralPath $transactionTestRoot) {
        Remove-Item -LiteralPath $transactionTestRoot -Recurse -Force
    }
    Remove-Variable -Name transactionDevices -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name transactionRestoreCalls -Scope Script -ErrorAction SilentlyContinue
}

$originalResolveVddPayload = ${function:Resolve-VddPayload}
$originalGetVddPaths = ${function:Get-VddPaths}
$originalGetVddState = ${function:Get-VddState}
$originalStageVddPayload = ${function:Stage-VddPayload}
$originalAddVddDriverPackage = ${function:Add-VddDriverPackage}
$originalRemoveAllVddDevices = ${function:Remove-AllVddDevices}
$originalRemoveVddRegistry = ${function:Remove-VddRegistry}
$originalCleanupVddPackages = ${function:Cleanup-VddPackages}
$originalSetVddConfiguration = ${function:Set-VddConfiguration}
$originalInstallVddDevice = ${function:Install-VddDevice}
$originalRestoreVddDevice = ${function:Restore-VddDevice}
$originalSaveVddTransaction = ${function:Save-VddTransaction}
$originalClearVddTransaction = ${function:Clear-VddTransaction}
try {
    function Resolve-VddPayload {
        return [pscustomobject]@{
            RawBuild = '22000'
            BuildNumber = 22000
            DriverDir = $PSScriptRoot
            ConfigSource = $HelperScript
            Paths = [pscustomobject]@{
                Nefcon = $HelperScript
                ConfigDir = $PSScriptRoot
            }
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
            Devices = @($script:workflowDevices)
            BundledVersion = '100.0.16.6'
            Decision = $script:workflowDecision
        }
    }
    function Stage-VddPayload { $script:workflowCalls += 'Stage' }
    function Add-VddDriverPackage {
        $script:workflowCalls += 'Package'
        if ($script:failWorkflowPackage) {
            throw 'simulated package validation failure'
        }
    }
    function Remove-AllVddDevices { $script:workflowCalls += 'Remove' }
    function Remove-VddRegistry { $script:workflowCalls += 'Registry' }
    function Cleanup-VddPackages([string] $ExpectedVersion = '') {
        $script:workflowCalls += "Cleanup:$ExpectedVersion"
    }
    function Set-VddConfiguration { $script:workflowCalls += 'Configure' }
    function Install-VddDevice {
        $script:workflowCalls += 'Install'
        if ($script:failWorkflowInstall) {
            throw 'simulated install failure'
        }
    }
    function Restore-VddDevice { $script:workflowCalls += 'Restore' }
    function Save-VddTransaction { $script:workflowCalls += 'Journal' }
    function Clear-VddTransaction { $script:workflowCalls += 'Clear' }

    $script:workflowCalls = @()
    $script:workflowDevices = @(New-TestDevice '100.0.16.6')
    $script:failWorkflowInstall = $false
    $script:failWorkflowPackage = $false
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 0
        InstallRequired = 0
        HealthyExisting = 1
        ControlAvailable = 1
        CurrentVersion = '100.0.16.6'
        CurrentStatus = 'OK'
        CurrentInf = 'oem42.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run'
    Assert-Equal 'Stage,Configure,Cleanup:100.0.16.6' ($script:workflowCalls -join ',') `
        'A healthy matching device must keep its package and skip reinstall.'

    $script:workflowCalls = @()
    $script:workflowDevices = @(New-TestDevice '100.0.16.5' 'OK' 'oem40.inf')
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 1
        InstallRequired = 1
        HealthyExisting = 1
        ControlAvailable = 1
        CurrentVersion = '100.0.16.5'
        CurrentStatus = 'OK'
        CurrentInf = 'oem40.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run'
    Assert-Equal 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' ($script:workflowCalls -join ',') `
        'An upgrade must stage the replacement before removing the working device and prune only after verification.'

    $script:workflowCalls = @()
    Invoke-VddInstall $PSScriptRoot 'Run' -PreserveHealthyExisting
    Assert-Equal 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' `
        ($script:workflowCalls -join ',') `
        'An unattended upgrade must replace a healthy older driver.'

    $script:workflowCalls = @()
    $script:workflowDevices = @(New-TestDevice '100.0.16.7' 'OK' 'oem43.inf')
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 1
        InstallRequired = 1
        HealthyExisting = 1
        ControlAvailable = 1
        CurrentVersion = '100.0.16.7'
        CurrentStatus = 'OK'
        CurrentInf = 'oem43.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run' -PreserveHealthyExisting
    Assert-Equal 'Stage,Configure' ($script:workflowCalls -join ',') `
        'An unattended upgrade must not downgrade a healthy newer driver.'

    $script:workflowCalls = @()
    $script:workflowDevices = @(New-TestDevice '100.0.16.6')
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 1
        InstallRequired = 1
        HealthyExisting = 1
        ControlAvailable = 0
        CurrentVersion = '100.0.16.6'
        CurrentStatus = 'OK'
        CurrentInf = 'oem42.inf'
    }
    Invoke-VddInstall $PSScriptRoot 'Run' -PreserveHealthyExisting
    Assert-Equal 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' `
        ($script:workflowCalls -join ',') `
        'An unattended upgrade must repair a driver whose required control interface is missing.'

    $script:workflowDevices = @(New-TestDevice '100.0.16.5' 'OK' 'oem40.inf')
    $script:workflowDecision = [pscustomobject]@{
        DeviceCount = 1
        CleanupRequired = 1
        InstallRequired = 1
        HealthyExisting = 1
        ControlAvailable = 1
        CurrentVersion = '100.0.16.5'
        CurrentStatus = 'OK'
        CurrentInf = 'oem40.inf'
    }
    $script:workflowCalls = @()
    $script:failWorkflowPackage = $true
    $packageValidationFailed = $false
    try {
        Invoke-VddInstall $PSScriptRoot 'Run'
    }
    catch {
        $packageValidationFailed = $_.Exception.Message -like '*simulated package validation failure*'
    }
    Assert-Equal $true $packageValidationFailed 'A rejected replacement package must abort the update.'
    Assert-Equal 'Stage,Configure,Package' ($script:workflowCalls -join ',') `
        'A rejected replacement package must not remove the working VDD.'
    $script:failWorkflowPackage = $false

    $script:workflowCalls = @()
    $script:failWorkflowInstall = $true
    $installFailed = $false
    try {
        Invoke-VddInstall $PSScriptRoot 'Run'
    }
    catch {
        $installFailed = $_.Exception.Message -like '*simulated install failure*'
    }
    Assert-Equal $true $installFailed 'A failed driver replacement must report the original failure.'
    Assert-Equal 'Stage,Configure,Package,Journal,Remove,Install,Restore,Clear' ($script:workflowCalls -join ',') `
        'A failed driver replacement must restore the previous healthy driver.'
    $script:failWorkflowInstall = $false

    $script:workflowCalls = @()
    Invoke-VddUninstall $PSScriptRoot
    Assert-Equal 'Remove,Cleanup:,Clear,Registry' ($script:workflowCalls -join ',') `
        'Uninstall must remove devices before packages and always clean the registry.'
}
finally {
    ${function:Resolve-VddPayload} = $originalResolveVddPayload
    ${function:Get-VddPaths} = $originalGetVddPaths
    ${function:Get-VddState} = $originalGetVddState
    ${function:Stage-VddPayload} = $originalStageVddPayload
    ${function:Add-VddDriverPackage} = $originalAddVddDriverPackage
    ${function:Remove-AllVddDevices} = $originalRemoveAllVddDevices
    ${function:Remove-VddRegistry} = $originalRemoveVddRegistry
    ${function:Cleanup-VddPackages} = $originalCleanupVddPackages
    ${function:Set-VddConfiguration} = $originalSetVddConfiguration
    ${function:Install-VddDevice} = $originalInstallVddDevice
    ${function:Restore-VddDevice} = $originalRestoreVddDevice
    ${function:Save-VddTransaction} = $originalSaveVddTransaction
    ${function:Clear-VddTransaction} = $originalClearVddTransaction
    Remove-Variable -Name workflowCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name workflowDecision -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name workflowDevices -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name failWorkflowInstall -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name failWorkflowPackage -Scope Script -ErrorAction SilentlyContinue
}

$results | Format-Table -AutoSize
Write-Host 'VDD helper smoke tests passed.'
