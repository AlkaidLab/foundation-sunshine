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

$nativeExitCode = Invoke-Nefcon $env:ComSpec @('/d', '/c', 'exit', '37')
Assert-Equal -Expected 37 -Actual $nativeExitCode -Message 'The native process wrapper must return the child exit code.'

$missingExitCodeRejected = $false
try {
    [void] (Invoke-Nefcon 'Write-Output' @('simulated non-native command'))
}
catch {
    $missingExitCodeRejected = $_.Exception.Message -like '*did not report an exit code*'
}
Assert-Equal -Expected $true -Actual $missingExitCodeRejected -Message 'A missing native exit code must fail explicitly.'

$originalInvokeNefcon = ${function:Invoke-Nefcon}
$originalGetVddDevices = ${function:Get-VddDevices}
$originalTestVddControlInterfaceAvailable = ${function:Test-VddControlInterfaceAvailable}
$originalWaitUntil = ${function:Wait-Until}
try {
    $script:installNefconCalls = 0
    $script:readinessChecks = 0
    $script:controlInterfaceChecks = 0
    $script:receivedWaitSeconds = 0
    $script:forceWaitTimeout = $false
    $script:installTestDevice = New-TestDevice '100.0.16.6'
    function Invoke-Nefcon([string] $Path, [string[]] $Arguments) {
        $script:installNefconCalls++
        if ($Arguments[0] -eq '--install-driver') {
            return 3010
        }
        return 0
    }
    function Get-VddDevices {
        $script:readinessChecks++
        return @($script:installTestDevice)
    }
    function Test-VddControlInterfaceAvailable {
        $script:controlInterfaceChecks++
        return $true
    }
    function Wait-Until([scriptblock] $Condition, [int] $WaitSeconds) {
        $script:receivedWaitSeconds = $WaitSeconds
        if ($script:forceWaitTimeout) {
            return $false
        }
        return [bool] (& $Condition)
    }

    [void] (Install-VddDeviceFromInf $HelperScript $HelperScript '100.0.16.6')
    Assert-Equal -Expected 2 -Actual $script:installNefconCalls `
        -Message 'A restart-suggested driver bind must continue to the readiness check.'
    Assert-Equal -Expected $true -Actual ($script:readinessChecks -ge 1) `
        -Message 'A restart-suggested driver bind must check device readiness.'
    Assert-Equal -Expected 120 -Actual $script:receivedWaitSeconds -Message `
        'VDD readiness checks must retain the 120-second timeout.'
    Assert-Equal -Expected $true -Actual ($script:controlInterfaceChecks -ge 1) -Message `
        'A ready VDD must verify that its control interface is available.'

    $script:forceWaitTimeout = $true
    $script:installTestDevice = New-TestDevice '100.0.16.6' 'ERROR'
    $timeoutResult = Install-VddDeviceFromInf $HelperScript $HelperScript '100.0.16.6'
    Assert-Equal -Expected $vddDeviceRestartRequired -Actual $timeoutResult -Message `
        'A timed-out device bound to the expected version must require a restart.'

    $script:vddRestartRequired = $false
    $script:installTestDevice = New-TestDevice '100.0.16.5' 'ERROR'
    $wrongVersionRejected = $false
    try {
        [void] (Install-VddDeviceFromInf $HelperScript $HelperScript '100.0.16.6')
    }
    catch {
        $wrongVersionRejected = $_.Exception.Message -like '*did not bind and become ready*version 100.0.16.6*'
    }
    Assert-Equal -Expected $true -Actual $wrongVersionRejected -Message `
        'A timed-out device bound to a different version must fail instead of requesting a restart.'
    Assert-Equal -Expected $false -Actual $script:vddRestartRequired -Message `
        'A wrong-version timeout must not request a restart.'
}
finally {
    ${function:Invoke-Nefcon} = $originalInvokeNefcon
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Test-VddControlInterfaceAvailable} = $originalTestVddControlInterfaceAvailable
    ${function:Wait-Until} = $originalWaitUntil
    Remove-Variable -Name installNefconCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name readinessChecks -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name controlInterfaceChecks -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name receivedWaitSeconds -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name forceWaitTimeout -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name installTestDevice -Scope Script -ErrorAction SilentlyContinue
    $script:vddRestartRequired = $false
}

Assert-Equal -Expected $false -Actual (Test-VddVersionAtLeast '100.0.16.5' '100.0.16.6') `
    -Message 'An older driver version must not satisfy the bundled minimum.'
Assert-Equal -Expected $true -Actual (Test-VddVersionAtLeast '100.0.16.7' '100.0.16.6') `
    -Message 'A newer driver version must satisfy the bundled minimum.'
Assert-Equal -Expected $true -Actual (Test-VddVersionAtLeast 'custom-build' '100.0.16.6') `
    -Message 'An incomparable healthy custom driver must be preserved.'
Assert-Equal -Expected $false -Actual (Test-VddVersionAtLeast '100.0.15.6' '15.0.15.7') `
    -Message 'A legacy 100.x Win10 driver must migrate to the safe 15.x version line.'
Assert-Equal -Expected $false -Actual (Test-VddVersionAtLeast '99.8.8.1123' '15.0.15.7') `
    -Message 'A legacy 99.x Win10 driver must migrate to the safe 15.x version line.'
Assert-Equal -Expected $true -Actual (Test-VddVersionAtLeast '15.0.15.8' '15.0.15.7') `
    -Message 'A newer safe Win10 driver must still be preserved.'
Assert-Equal -Expected $true -Actual (Test-VddVersionAtLeast '100.0.17.2' '100.0.17.2') `
    -Message 'The Win11 100.x version policy must remain unchanged.'
Assert-Equal -Expected $true -Actual (Test-VddDeviceBound (New-TestDevice '15.0.15.8' 'ERROR') '15.0.15.8') `
    -Message 'A non-started device with the expected published package must be eligible for reboot recovery.'
Assert-Equal -Expected $false -Actual (Test-VddDeviceBound (New-TestDevice '15.0.15.7') '15.0.15.8') `
    -Message 'A device bound to the wrong package must not be treated as a reboot-only result.'

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
    Assert-Equal -Expected $case.Count -Actual $decision.DeviceCount -Message "$($case.Name): wrong device count"
    Assert-Equal -Expected $case.Cleanup -Actual $decision.CleanupRequired -Message "$($case.Name): wrong cleanup decision"
    Assert-Equal -Expected $case.Install -Actual $decision.InstallRequired -Message "$($case.Name): wrong install decision"
    Assert-Equal -Expected ([int]$controlAvailable) -Actual $decision.ControlAvailable `
        -Message "$($case.Name): wrong control-interface decision"

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
Assert-Equal -Expected 2 -Actual $stalePackages.Count -Message 'Package pruning must keep exactly the active OEM INF.'
Assert-Equal -Expected 'oem40.inf,oem43.inf' -Actual ($stalePackages.InfName -join ',') -Message 'Wrong stale package selection.'

$invalidKeepWasRejected = $false
try {
    [void] (Get-VddPackagesToRemove $packages 'ZakoVDD.inf')
}
catch {
    $invalidKeepWasRejected = $true
}
Assert-Equal -Expected $true -Actual $invalidKeepWasRejected -Message 'Package pruning must reject a non-published INF name.'

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
    Assert-Equal -Expected 1 -Actual $foundPackages.Count -Message 'An unreadable unrelated INF must not abort the package scan.'
    Assert-Equal -Expected 'oem42.inf' -Actual $foundPackages[0].InfName -Message 'The readable VDD package must still be detected.'
    Assert-Equal -Expected 'Root\ZakoVDD,ZakoVDD' -Actual ($script:lastInfPatterns -join ',') `
        -Message 'Package discovery must recognize current and legacy hardware IDs.'

    $activeReadFailedClosed = $false
    try {
        [void] (Get-PublishedVddPackages -RequiredInfNames @('oem40.inf'))
    }
    catch {
        $activeReadFailedClosed = $_.Exception.Message -like '*active VDD package oem40.inf*'
    }
    Assert-Equal -Expected $true -Actual $activeReadFailedClosed -Message 'An unreadable active VDD package must abort the scan.'
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
    Assert-Equal -Expected $true -Actual $probeFailedClosed -Message 'A device enumeration failure must abort the probe.'
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
    Assert-Equal -Expected $win32ErrorTimeout -Actual $timedOutExitCode `
        -Message 'A hung pnputil process must return the Windows timeout error.'
    Assert-Equal -Expected 1 -Actual $script:stoppedPnpProcesses `
        -Message 'A hung pnputil process must be force-stopped.'

    $script:fakePnpProcess = [pscustomobject]@{
        HasExited = $true
        ExitCode = 3010
    }
    $script:pnpWaitTimesOut = $false
    $completedExitCode = Invoke-PnpUtilDeviceRemoval 'ROOT\DISPLAY\0042'
    Assert-Equal -Expected 3010 -Actual $completedExitCode `
        -Message 'A completed pnputil process must preserve its exit code.'
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
    Assert-Equal -Expected 0 -Actual $script:fakeDevices.Count -Message 'All duplicate device nodes must be removed.'
    Assert-Equal -Expected 1 -Actual $script:removalCalls -Message 'An asynchronous removal must not receive overlapping requests.'
    Assert-Equal -Expected 0 -Actual $script:targetedRemovalCalls -Message 'Live nodes must not receive overlapping targeted removal requests.'

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
    Assert-Equal -Expected 2 -Actual $script:removalCalls -Message 'Root and legacy hardware IDs must both be reconciled.'
    Assert-Equal -Expected 'Root\ZakoVDD,ZakoVDD' -Actual ($script:removedHardwareIds -join ',') `
        -Message 'Removal must target both managed hardware IDs.'
    Assert-Equal -Expected 1 -Actual $script:targetedRemovalCalls -Message 'A disconnected legacy node must receive targeted cleanup.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6' 'ERROR')
    $script:removalCalls = 0
    $script:targetedRemovalCalls = 0
    function Get-VddDevices {
        if ($script:targetedRemovalCalls -gt 0) {
            return @()
        }
        return @($script:fakeDevices)
    }
    function Invoke-NefconRemoval([string] $Path, [string] $TargetHardwareId) {
        $script:removalCalls++
        return 0
    }

    [void] (Remove-AllVddDevices $HelperScript 2)
    Assert-Equal -Expected 1 -Actual $script:removalCalls -Message 'An error-state node must first receive the normal removal request.'
    Assert-Equal -Expected 1 -Actual $script:targetedRemovalCalls -Message 'An error-state node must receive targeted cleanup.'

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
    Assert-Equal -Expected $true -Actual $removalFailedClosed -Message 'A failed nefcon request must abort immediately.'
    Assert-Equal -Expected 1 -Actual $script:removalCalls -Message 'A stalled removal must not repeatedly invoke nefcon.'

    $script:fakeDevices = @(New-TestDevice '100.0.16.6' 'ERROR')
    $script:removalCalls = 0
    $script:targetedRemovalCalls = 0
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
    Assert-Equal -Expected $true -Actual $removalMadeNoProgress -Message 'A successful request without PnP progress must fail with diagnostics.'
    Assert-Equal -Expected 1 -Actual $script:removalCalls -Message 'A stalled removal must not repeatedly invoke nefcon.'
    Assert-Equal -Expected 1 -Actual $script:targetedRemovalCalls -Message 'A stalled error-state node must receive one targeted cleanup request.'

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
    Assert-Equal -Expected $true -Actual $liveRemovalTimedOut -Message 'A live asynchronous removal must keep the normal timeout path.'
    Assert-Equal -Expected 0 -Actual $script:targetedRemovalCalls -Message 'A live node must not receive the disconnected-node fallback.'
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
    $script:transactionRestoreResult = $vddDeviceReady
    function Get-VddDevices { return @($script:transactionDevices) }
    function Restore-VddDevice {
        $script:transactionRestoreCalls++
        return $script:transactionRestoreResult
    }

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    Recover-VddTransaction $transactionPayload '100.0.16.6'
    Assert-Equal -Expected 1 -Actual $script:transactionRestoreCalls `
        -Message 'An interrupted replacement without a ready VDD must restore the previous package.'
    Assert-Equal -Expected $false -Actual (Test-Path -LiteralPath (Get-VddTransactionPath $transactionPayload)) `
        -Message 'A recovered VDD transaction must be cleared.'

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    $script:transactionRestoreResult = $vddDeviceRestartRequired
    $recoveryResult = Recover-VddTransaction $transactionPayload '100.0.16.6'
    Assert-Equal -Expected $vddDeviceRestartRequired -Actual $recoveryResult -Message `
        'A rollback that binds but is not ready must propagate the restart requirement.'
    Assert-Equal -Expected $true `
        -Actual (Test-Path -LiteralPath (Get-VddTransactionPath $transactionPayload)) `
        -Message `
        'A reboot-pending rollback must preserve its update transaction.'
    Assert-Equal -Expected $false -Actual $script:vddRestartRequired -Message `
        'Transaction recovery must propagate the internal result without relying on a test-double side effect.'
    Clear-VddTransaction $transactionPayload
    $script:transactionRestoreResult = $vddDeviceReady
    $script:vddRestartRequired = $false

    Save-VddTransaction $transactionPayload $transactionPreviousDevice
    $script:transactionDevices = @(New-TestDevice '100.0.16.6')
    Recover-VddTransaction $transactionPayload '100.0.16.6'
    Assert-Equal -Expected 2 -Actual $script:transactionRestoreCalls -Message `
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
    Assert-Equal -Expected $true -Actual $unexpectedDriverWasRejected `
        -Message 'Recovery must not replace a different healthy VDD automatically.'
}
finally {
    ${function:Get-VddDevices} = $originalGetVddDevices
    ${function:Restore-VddDevice} = $originalRestoreVddDevice
    if (Test-Path -LiteralPath $transactionTestRoot) {
        Remove-Item -LiteralPath $transactionTestRoot -Recurse -Force
    }
    Remove-Variable -Name transactionDevices -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name transactionRestoreCalls -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name transactionRestoreResult -Scope Script -ErrorAction SilentlyContinue
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
        if ($script:workflowRequestsRestart) {
            return $vddDeviceRestartRequired
        }
        return $vddDeviceReady
    }
    function Restore-VddDevice {
        $script:workflowCalls += 'Restore'
        if ($script:workflowRestoreRequestsRestart) {
            return $vddDeviceRestartRequired
        }
        return $vddDeviceReady
    }
    function Save-VddTransaction { $script:workflowCalls += 'Journal' }
    function Clear-VddTransaction { $script:workflowCalls += 'Clear' }

    $script:workflowCalls = @()
    $script:workflowDevices = @(New-TestDevice '100.0.16.6')
    $script:failWorkflowInstall = $false
    $script:failWorkflowPackage = $false
    $script:workflowRequestsRestart = $false
    $script:workflowRestoreRequestsRestart = $false
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
    Assert-Equal -Expected 'Stage,Configure,Cleanup:100.0.16.6' -Actual ($script:workflowCalls -join ',') `
        -Message 'A healthy matching device must keep its package and skip reinstall.'

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
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' -Actual ($script:workflowCalls -join ',') `
        -Message 'An upgrade must stage the replacement before removing the working device and prune only after verification.'

    $script:workflowCalls = @()
    $script:workflowRequestsRestart = $true
    Invoke-VddInstall $PSScriptRoot 'Run'
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install' -Actual ($script:workflowCalls -join ',') `
        -Message 'A reboot-pending upgrade must preserve its rollback journal and previous driver package.'
    Assert-Equal -Expected $true -Actual $script:vddRestartRequired `
        -Message 'A reboot-pending upgrade must propagate the restart requirement to the caller.'
    $script:workflowRequestsRestart = $false
    $script:vddRestartRequired = $false

    $script:workflowCalls = @()
    Invoke-VddInstall $PSScriptRoot 'Run' -PreserveHealthyExisting
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' `
        -Actual ($script:workflowCalls -join ',') `
        -Message 'An unattended upgrade must replace a healthy older driver.'

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
    Assert-Equal -Expected 'Stage,Configure' -Actual ($script:workflowCalls -join ',') `
        -Message 'An unattended upgrade must not downgrade a healthy newer driver.'

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
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install,Clear,Cleanup:100.0.16.6' `
        -Actual ($script:workflowCalls -join ',') `
        -Message 'An unattended upgrade must repair a driver whose required control interface is missing.'

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
    Assert-Equal -Expected $true -Actual $packageValidationFailed -Message 'A rejected replacement package must abort the update.'
    Assert-Equal -Expected 'Stage,Configure,Package' -Actual ($script:workflowCalls -join ',') `
        -Message 'A rejected replacement package must not remove the working VDD.'
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
    Assert-Equal -Expected $true -Actual $installFailed -Message 'A failed driver replacement must report the original failure.'
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install,Restore,Clear' -Actual ($script:workflowCalls -join ',') `
        -Message 'A failed driver replacement must restore the previous healthy driver.'

    $script:workflowCalls = @()
    $script:workflowRestoreRequestsRestart = $true
    $script:vddRestartRequired = $false
    $rollbackPendingThrew = $false
    try {
        Invoke-VddInstall $PSScriptRoot 'Run'
    }
    catch {
        $rollbackPendingThrew = $true
    }
    Assert-Equal -Expected $false -Actual $rollbackPendingThrew -Message `
        'A reboot-pending rollback must propagate 3010 instead of the original install failure.'
    Assert-Equal -Expected 'Stage,Configure,Package,Journal,Remove,Install,Restore' `
        -Actual ($script:workflowCalls -join ',') `
        -Message `
        'A reboot-pending rollback must stop before clearing the update transaction.'
    Assert-Equal -Expected $true -Actual $script:vddRestartRequired -Message `
        'A reboot-pending rollback must propagate the restart requirement to the caller.'
    $script:failWorkflowInstall = $false
    $script:workflowRestoreRequestsRestart = $false
    $script:vddRestartRequired = $false

    $script:workflowCalls = @()
    Invoke-VddUninstall $PSScriptRoot
    Assert-Equal -Expected 'Remove,Cleanup:,Clear,Registry' -Actual ($script:workflowCalls -join ',') `
        -Message 'Uninstall must remove devices before packages and always clean the registry.'
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
    Remove-Variable -Name workflowRequestsRestart -Scope Script -ErrorAction SilentlyContinue
    Remove-Variable -Name workflowRestoreRequestsRestart -Scope Script -ErrorAction SilentlyContinue
    $script:vddRestartRequired = $false
}

$results | Format-Table -AutoSize
Write-Host 'VDD helper smoke tests passed.'
