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

function Assert-ProtectedFolder([string]$Folder, [string]$Phase) {
    # The service runs the programs of the install folder as SYSTEM, so outside Program Files the folder has the protection of Program Files (HardenInstallDir in
    # sunshine.iss.in): no inheritance, owner Administrators, and nothing but read and execute for anybody who is not SYSTEM, Administrators or TrustedInstaller.
    # Authority: installer-ms-icacls-20261119, installer-ms-well-known-sids-20261119, installer-ms-filesystemrights-20261119.
    $trusted = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    $acl = Get-Acl -LiteralPath $Folder
    if (-not $acl.AreAccessRulesProtected) { throw "$Phase`: $Folder still inherits permissions from its parent." }
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -ne 'S-1-5-32-544') { throw "$Phase`: the owner of $Folder is $owner, not Administrators." }
    $write = [Security.AccessControl.FileSystemRights]::WriteData -bor [Security.AccessControl.FileSystemRights]::AppendData -bor
        [Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor [Security.AccessControl.FileSystemRights]::WriteAttributes -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -ne 'Allow' -or $trusted -contains $rule.IdentityReference.Value) { continue }
        if (($rule.FileSystemRights -band $write) -ne 0) {
            throw "$Phase`: $($rule.IdentityReference.Value) can write to $Folder ($($rule.FileSystemRights), inherited: $($rule.IsInherited)).`n$(icacls $Folder | Out-String)"
        }
    }
    # Users can read and run what is in the folder: an allow entry that applies to the folder itself (not inherit-only) carrying ReadAndExecute
    $readExecute = [Security.AccessControl.FileSystemRights]::ReadAndExecute
    $users = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | Where-Object {
        $_.IdentityReference.Value -eq 'S-1-5-32-545' -and $_.AccessControlType -eq 'Allow' -and
        ($_.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -eq 0 -and
        ($_.FileSystemRights -band $readExecute) -eq $readExecute })
    if (-not $users.Count) { throw "$Phase`: Users cannot read and run what is in $Folder." }
    # a file the install created below it inherits that
    $exe = Get-Acl -LiteralPath (Join-Path $Folder 'sunshine.exe')
    $exeOwner = $exe.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($trusted -notcontains $exeOwner) { throw "$Phase`: the owner of sunshine.exe is $exeOwner, who could change its permissions." }
    foreach ($rule in $exe.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and $trusted -notcontains $rule.IdentityReference.Value -and ($rule.FileSystemRights -band $write) -ne 0) {
            throw "$Phase`: $($rule.IdentityReference.Value) can write to sunshine.exe."
        }
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


function Invoke-InstallerExpecting([string]$Arguments, [string]$Phase, [scriptblock]$ExitCodeOk) {
    # An update that must be refused or that fails on purpose: the exit code is part of the contract.
    $process = Start-Process -FilePath $Installer -ArgumentList $Arguments -PassThru
    $record = [pscustomobject]@{ phase = $Phase; pid = $process.Id; exit_code = $null; passed = $false }
    $results.Add($record)
    Write-Host "$Phase started PID $($process.Id)"
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            throw "$Phase timed out after $TimeoutSeconds seconds (PID $($process.Id))."
        }
        $record.exit_code = $process.ExitCode
        if (-not (& $ExitCodeOk $process.ExitCode)) { throw "$Phase exited $($process.ExitCode), which is not what the contract allows." }
        $record.passed = $true
    } finally {
        $process.Dispose()
    }
}

function Get-Manifest([string]$Folder, [string[]]$Relative) {
    # Relative path -> SHA-256 of every file below the given entries.
    $manifest = [ordered]@{}
    # The runner's TEMP can be spelled with 8.3 names (RUNNER~1) while Get-ChildItem returns the long
    # ones, so cut the prefix off the resolved folder, not off the text the caller passed.
    $resolved = (Get-Item -LiteralPath $Folder).FullName.TrimEnd('\')
    foreach ($item in $Relative) {
        $path = Join-Path $Folder $item
        if (Test-Path -LiteralPath $path -PathType Container) {
            Get-ChildItem -LiteralPath $path -Recurse -File | Sort-Object FullName | ForEach-Object {
                $manifest[$_.FullName.Substring($resolved.Length).TrimStart('\')] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
            $manifest[$item] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
    }
    return $manifest
}

function Assert-SameManifest($Expected, $Actual, [string]$Phase, [string]$What) {
    $a = ConvertTo-Json -InputObject $Expected -Depth 3
    $b = ConvertTo-Json -InputObject $Actual -Depth 3
    if ($a -ne $b) {
        $missing = @($Expected.Keys | Where-Object { -not $Actual.Contains($_) })
        $changed = @($Expected.Keys | Where-Object { $Actual.Contains($_) -and $Actual[$_] -ne $Expected[$_] })
        throw "$Phase`: $What differs from before the update ($($missing.Count) missing, $($changed.Count) changed, $($Actual.Count) of $($Expected.Count) present)."
    }
}

function Stop-TestService([string]$Phase) {
    # Only the service this install created. Authority: installer-ms-stop-service-20261118 (-NoWait).
    $service = Get-Service -Name SunshineService -ErrorAction SilentlyContinue
    if (-not $service -or $service.Status -eq 'Stopped') { return }
    Stop-Service -Name SunshineService -NoWait
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Get-Service -Name SunshineService).Status -ne 'Stopped') {
        if ([DateTime]::UtcNow -ge $deadline) { throw "$Phase`: the test service did not stop." }
        Start-Sleep -Milliseconds 200
    }
}

function Assert-NothingParked([string]$Folder, [string]$Phase) {
    # The programs Setup moves aside while it works are gone again, deleted or put back.
    $left = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Filter '*.parked' -ErrorAction SilentlyContinue)
    if ($left.Count) { throw "$Phase`: files moved aside were left behind: $($left.Name -join ', ')." }
}

function Stop-PathProcesses([string]$Path) {
    # By image path, so only what this test started from the folder under test.
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -ieq $Path } | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Test-UpdateSafety {
    # The update that failed on a user's machine: the Control Panel's own updater ran the installer
    # silently while a program of the folder still ran, the copy of sunshine-gui.exe failed, Setup
    # rolled back, and the rollback removed the Web UI folder it had just created. Every case below
    # ends with the Web UI folder of the installation that was there before.
    $app = Join-Path $testRoot 'update safety'
    $common = '/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /LANG=english /TYPE=custom /COMPONENTS=application,assets,gui /TASKS= ' + "/DIR=`"$app`""
    $kept = @('assets\web', 'sunshine.exe', 'tools\sunshinesvc.exe', 'scripts', 'assets\gui\sunshine-gui.exe')
    $gui = Join-Path $app 'assets\gui\sunshine-gui.exe'
    $previous = Join-Path $app 'assets\web.previous'
    $supervisor = $null
    $lock = $null
    $blocker = Join-Path $app 'scripts\update-path.bat'
    $blockerBackup = Join-Path $testRoot 'update-path.bat.kept'
    try {
        Invoke-BoundedProcess -File $Installer -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-fresh.log')`"" -Phase 'update-fresh'
        Stop-TestService 'update-fresh'
        # the service started the real Control Panel of this folder: it holds the file the next steps replace
        Stop-PathProcesses $gui
        Start-Sleep -Seconds 1
        $before = Get-Manifest $app $kept
        if (-not (Test-Path -LiteralPath (Join-Path $app 'assets\web\index.html'))) { throw 'update-fresh: no Web UI folder to protect.' }

        # A. A program of the folder is running (what the updater of an older Control Panel leaves behind).
        Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\ping.exe') -Destination $gui -Force
        $stub = Start-Process -FilePath $gui -ArgumentList '-t', '127.0.0.1' -PassThru -WindowStyle Hidden
        Invoke-BoundedProcess -File $Installer -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-running.log')`"" -Phase 'update-running-program'
        if (-not $stub.HasExited) { throw 'update-running-program: the program of the folder was not ended.' }
        Stop-TestService 'update-running-program'
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-running-program' 'the installed files'
        if (Test-Path -LiteralPath $previous) { throw 'update-running-program: the old Web UI folder was left behind.' }
        Assert-NothingParked $app 'update-running-program'

        # B. A file of the folder is held by something else: refused before anything changes.
        $lock = [IO.File]::Open($blocker, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        Invoke-InstallerExpecting -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-held.log')`"" -Phase 'update-held-file' -ExitCodeOk { param($code) $code -eq 7 }
        $lock.Dispose(); $lock = $null
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-held-file' 'the installed files after a refused update'
        if (Test-Path -LiteralPath $previous) { throw 'update-held-file: a refused update moved the Web UI folder aside.' }
        Assert-NothingParked $app 'update-held-file'

        # C. The program of the folder is started again as fast as it is ended (what the service does
        #    with the tray). The programs are moved aside once they are ended, so there is nothing
        #    to start, and the update goes through.
        Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\ping.exe') -Destination $gui -Force
        $guard = "while (`$true) { if (-not (Get-Process -Name 'sunshine-gui' -ErrorAction SilentlyContinue)) { Start-Process -FilePath '$gui' -ArgumentList '-t','127.0.0.1' -WindowStyle Hidden -ErrorAction SilentlyContinue }; Start-Sleep -Milliseconds 700 }"
        $supervisor = Start-Process -FilePath (Get-Command pwsh).Source -ArgumentList '-NoProfile', '-Command', $guard -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 2
        Invoke-BoundedProcess -File $Installer -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-respawn.log')`"" -Phase 'update-respawning-program'
        # the supervisor goes first: it starts the installed program as soon as the new one is in place
        $supervisor | Stop-Process -Force
        $supervisor = $null
        Stop-PathProcesses $gui
        Stop-TestService 'update-respawning-program'
        Start-Sleep -Seconds 1
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-respawning-program' 'the installed files'
        if (Test-Path -LiteralPath $previous) { throw 'update-respawning-program: the old Web UI folder was left behind.' }
        Assert-NothingParked $app 'update-respawning-program'

        # D. The copy fails after the Web UI folder was moved aside (a directory stands where a file
        #    has to go). Setup rolls back; the old folder must be back.
        Move-Item -LiteralPath $blocker -Destination $blockerBackup -Force
        New-Item -ItemType Directory -Path $blocker -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $blocker 'occupied.txt') -Value 'a folder where the installer needs a file'
        $failingLog = Join-Path $EvidenceDirectory 'update-failing-copy.log'
        Invoke-InstallerExpecting -Arguments "$common /LOG=`"$failingLog`"" -Phase 'update-failing-copy' -ExitCodeOk { param($code) $code -ne 0 -and $code -ne 7 }
        Stop-TestService 'update-failing-copy'
        # the failure has to be the one meant: Setup rolled back after it had moved the folder aside, and put it back
        if (-not (Select-String -LiteralPath $failingLog -SimpleMatch 'Rolling back changes' -Quiet)) { throw 'update-failing-copy: Setup did not roll back, so the copy did not fail where it was meant to.' }
        if (-not (Select-String -LiteralPath $failingLog -SimpleMatch 'the previous Web UI folder was put back' -Quiet)) { throw 'update-failing-copy: the log does not show the Web UI folder being put back.' }
        Remove-Item -LiteralPath $blocker -Recurse -Force
        Move-Item -LiteralPath $blockerBackup -Destination $blocker -Force
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-failing-copy' 'the installed files after a failed update'
        $webBefore = [ordered]@{}
        foreach ($key in @($before.Keys)) { if (([string]$key).StartsWith('assets\web\', [StringComparison]::OrdinalIgnoreCase)) { $webBefore[$key] = $before[$key] } }
        if (Test-Path -LiteralPath $previous) { throw 'update-failing-copy: the old Web UI folder was left beside the restored one.' }
        Assert-NothingParked $app 'update-failing-copy'

        # E. A Setup that was killed after it moved the folder aside: the next update puts it back
        #    and finishes the job.
        Move-Item -LiteralPath (Join-Path $app 'assets\web') -Destination $previous -Force
        Invoke-BoundedProcess -File $Installer -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-after-kill.log')`"" -Phase 'update-after-interrupted-run'
        Stop-TestService 'update-after-interrupted-run'
        if (-not (Select-String -LiteralPath (Join-Path $EvidenceDirectory 'update-after-kill.log') -SimpleMatch 'Restored the Web UI folder left aside by an earlier update' -Quiet)) { throw 'update-after-interrupted-run: the log does not show the folder left aside being put back.' }
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-after-interrupted-run' 'the installed files'
        if (Test-Path -LiteralPath $previous) { throw 'update-after-interrupted-run: the old Web UI folder was left behind.' }

        # F. A Setup that was killed in the middle of the copy: the live folder has pages and a script
        #    but no end marker, the old one is beside it. The old one is the complete one.
        Copy-Item -LiteralPath (Join-Path $app 'assets\web') -Destination $previous -Recurse
        Remove-Item -LiteralPath (Join-Path $app 'assets\web\.install-complete') -Force
        Get-ChildItem -LiteralPath (Join-Path $app 'assets\web\assets') -Recurse -File | Where-Object { $_.Extension -ne '.js' } | Select-Object -First 5 | Remove-Item -Force
        Invoke-BoundedProcess -File $Installer -Arguments "$common /LOG=`"$(Join-Path $EvidenceDirectory 'update-after-half-copy.log')`"" -Phase 'update-after-half-copy'
        Stop-TestService 'update-after-half-copy'
        if (-not (Select-String -LiteralPath (Join-Path $EvidenceDirectory 'update-after-half-copy.log') -SimpleMatch 'Restored the Web UI folder left aside by an earlier update' -Quiet)) { throw 'update-after-half-copy: the log does not show the old folder being put back over the unfinished one.' }
        Assert-SameManifest $before (Get-Manifest $app $kept) 'update-after-half-copy' 'the installed files'
        if (Test-Path -LiteralPath $previous) { throw 'update-after-half-copy: the old Web UI folder was left behind.' }

        $results.Add([pscustomobject]@{ phase = 'update-safety'; passed = $true; web_files = $webBefore.Count })
        Write-Host "PASS update safety: a running program is ended, a held file is refused before any change, a program that is started again as fast as it is ended cannot get in the way, a failed copy, a run killed after it moved the folder aside and a run killed in the middle of the copy leave the Web UI folder ($($webBefore.Count) files) as it was."
        $uninstallLog = Join-Path $EvidenceDirectory 'update-safety-uninstall.log'
        Invoke-BoundedProcess -File (Join-Path $app 'unins000.exe') -Arguments "/VERYSILENT /NORESTART /LOG=`"$uninstallLog`"" -Phase 'update-safety-uninstall'
        Wait-UninstallSecondPhase -AppDir $app -Phase 'update-safety-uninstall'
    } finally {
        if ($lock) { $lock.Dispose() }
        if ($supervisor) { $supervisor | Stop-Process -Force -ErrorAction SilentlyContinue }
        Stop-PathProcesses $gui
    }
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
        Assert-ProtectedFolder -Folder $app -Phase "$mode-install"

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
    # The update path: what a failed or refused update leaves behind. A fresh clean machine again.
    Test-UpdateSafety
    Assert-CleanMachine
} finally {
    ConvertTo-Json -InputObject $results.ToArray() -Depth 5 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding utf8
}
