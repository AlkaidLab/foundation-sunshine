#Requires -Version 7.0
#Requires -RunAsAdministrator
<#
Exercise the production Inno engine on a disposable Windows runner. Do not pass
/SUPPRESSMSGBOXES: either silent switch must handle the script's own dialogs.
Authority: installer-inno-setup-cli-20261118 (/SILENT, /COMPONENTS, /NORESTART),
installer-inno-uninstall-cli-20261118 (silent switches and exit),
installer-ms-start-process-20261118 (ArgumentList, PassThru),
installer-ms-wait-for-exit-20261118 (WaitForExit(Int32)).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Installer,
    [string]$EvidenceDirectory = (Join-Path $env:TEMP 'installer-silent-evidence'),
    [ValidateRange(10, 300)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This smoke check requires Windows.' }
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$results = [Collections.Generic.List[object]]::new()
$installKey = 'SOFTWARE\AlkaidLab\Sunshine'
$uninstallRoot = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
$appKey = '{B16E5C0D-7B8A-4E3B-9F1A-2D4C6E8F0A1B}_is1'

function Assert-CleanMachine {
    # The real uninstaller includes system-wide cleanup. Never aim it at a
    # developer's installation, a shared service or an unrelated portable run.
    foreach ($hive in @('HKLM:', 'HKCU:')) {
        foreach ($view in @('', 'WOW6432Node\')) {
            foreach ($key in @(
                "$hive\SOFTWARE\${view}Microsoft\Windows\CurrentVersion\Uninstall\$appKey",
                "$hive\SOFTWARE\${view}Microsoft\Windows\CurrentVersion\Uninstall\Sunshine",
                "$hive\SOFTWARE\${view}AlkaidLab\Sunshine"
            )) {
                if (Test-Path -LiteralPath $key) {
                    # An empty vendor key can survive uninsdeletevalue.
                    $entry = Get-ItemProperty -LiteralPath $key
                    if ($key -notlike '*AlkaidLab\Sunshine' -or
                        $entry.PSObject.Properties.Name -contains 'InstallDir') {
                        throw "Existing installation metadata: $key. Use a disposable clean runner."
                    }
                }
            }
        }
    }
    foreach ($service in @('SunshineService', 'sunshinesvc')) {
        if (Get-Service -Name $service -ErrorAction SilentlyContinue) {
            throw "Existing service: $service. Use a disposable clean runner."
        }
    }
    $running = @(Get-Process -Name sunshine, sunshine-gui, sunshinesvc, sunshine-plugin-mat-capture -ErrorAction SilentlyContinue)
    if ($running.Count) { throw "Existing application processes: $($running.Id -join ',')." }
}

function Invoke-BoundedProcess([string]$File, [string]$Arguments, [string]$Phase) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru
    $record = [pscustomobject]@{
        phase = $Phase; pid = $process.Id; exit_code = $null; seconds = $null; passed = $false
    }
    $results.Add($record)
    Write-Host "$Phase started PID $($process.Id)"
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # Leave the recorded PID for runner teardown; never terminate by name.
            throw "$Phase timed out after $TimeoutSeconds seconds (PID $($process.Id))."
        }
        $record.exit_code = $process.ExitCode
        if ($process.ExitCode -ne 0) { throw "$Phase exited $($process.ExitCode)." }
        $record.passed = $true
    } finally {
        $record.seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        $process.Dispose()
    }
}

function Wait-UninstallSecondPhase([string]$AppDir, [string]$Phase) {
    # The launching uninstaller exits 0 and removes itself once it has started the second-phase copy; the script's
    # own prompts (usUninstall, usPostUninstall) run in that copy. Waiting for the launcher alone would let a prompt that
    # blocks the copy pass: binaries and registration are already gone and the user files are still untouched.
    # Authority: installer-inno-uninstall-cli-20261118 (silent switches and exit); installer-ms-wait-for-exit-20261118.
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $pending = @(Get-CimInstance -ClassName Win32_Process | Where-Object {
            $_.CommandLine -and $_.CommandLine -like '*/SECONDPHASE=*' -and $_.CommandLine -like "*$AppDir*"
        })
        if (-not $pending.Count) { return }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    # Leave the recorded PIDs for runner teardown; never terminate by name.
    throw "$Phase second phase still running after $TimeoutSeconds seconds (PID $($pending.ProcessId -join ',')); an uninstall prompt is probably waiting for input."
}

try {
    Assert-CleanMachine
    # Spaces exercise Inno's command-line quoting. Keep logs outside the
    # installed tree so uninstall failures remain inspectable as artifacts.
    $testRoot = Join-Path $env:TEMP ("silent installer smoke " + [Guid]::NewGuid().ToString('N'))
    foreach ($mode in @('SILENT', 'VERYSILENT')) {
        $app = Join-Path $testRoot $mode
        $installLog = Join-Path $EvidenceDirectory "$mode-install.log"
        $uninstallLog = Join-Path $EvidenceDirectory "$mode-uninstall.log"
        $arguments = "/$mode /SP- /NORESTART /LANG=english /TYPE=custom /COMPONENTS=application,assets,gui /TASKS= /DIR=`"$app`" /LOG=`"$installLog`""
        Invoke-BoundedProcess -File $Installer -Arguments $arguments -Phase "$mode-install"
        $binaries = @('sunshine.exe', 'tools\sunshinesvc.exe', 'assets\gui\sunshine-gui.exe', 'unins000.exe')
        foreach ($file in $binaries) {
            if (-not (Test-Path -LiteralPath (Join-Path $app $file) -PathType Leaf)) {
                throw "$mode did not install $file."
            }
        }
        $registered = Get-ItemPropertyValue -LiteralPath "HKLM:\$installKey" -Name InstallDir
        if ($registered.TrimEnd('\') -ine $app.TrimEnd('\')) { throw "Unexpected registered directory: $registered" }
        if (-not (Test-Path -LiteralPath "HKLM:\$uninstallRoot\$appKey")) { throw 'Missing uninstall registration.' }

        # Stop only the service created by this install before hashing fixtures,
        # so background first-run configuration writes cannot race the check.
        # Authority: installer-ms-stop-service-20261118 (-NoWait).
        Stop-Service -Name SunshineService -NoWait
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while ((Get-Service -Name SunshineService).Status -ne 'Stopped') {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'The test service did not stop.' }
            Start-Sleep -Milliseconds 200
        }
        $fixtures = [ordered]@{
            'config\sunshine.conf' = "# silent-uninstall retention sentinel`n"
            'config\apps.json' = '{"apps":[],"env":{}}'
            'config\sunshine_state.json' = '{"root":{"uniqueid":"silent-uninstall-retention-sentinel","named_devices":[]}}'
            'config\covers\silent-smoke.png' = 'cover retention sentinel'
        }
        $hashes = @{}
        foreach ($file in $fixtures.Keys) {
            $path = Join-Path $app $file
            New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
            [IO.File]::WriteAllText($path, $fixtures[$file])
            $hashes[$file] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
        $uninstaller = Join-Path $app 'unins000.exe'
        Invoke-BoundedProcess -File $uninstaller -Arguments "/$mode /NORESTART /LOG=`"$uninstallLog`"" -Phase "$mode-uninstall"
        Wait-UninstallSecondPhase -AppDir $app -Phase "$mode-uninstall"
        # Self-deletion may finish just after the second phase ends. Bound that wait too.
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $remaining = @($binaries | Where-Object { Test-Path -LiteralPath (Join-Path $app $_) })
            if (-not $remaining.Count) { break }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($remaining.Count) { throw "$mode left installed binaries: $($remaining -join ',')." }
        foreach ($file in $hashes.Keys) {
            $after = (Get-FileHash -LiteralPath (Join-Path $app $file) -Algorithm SHA256).Hash
            if ($after -cne $hashes[$file]) { throw "$mode changed retained user data: $file" }
        }
        Assert-CleanMachine
        $results.Add([pscustomobject]@{ phase = "$mode-retention"; passed = $true; files = $hashes })
        Write-Host "PASS ${mode}: install/uninstall exit 0; binaries and registration removed; four user files unchanged."
    }
} finally {
    ConvertTo-Json -InputObject $results.ToArray() -Depth 5 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding utf8
}
