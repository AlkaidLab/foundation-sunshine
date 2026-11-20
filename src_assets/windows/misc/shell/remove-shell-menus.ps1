param(
    # When copied into an installation directory, running the script without
    # arguments targets that directory. Uninstallers pass their root explicitly.
    [string]$InstallRoot = $PSScriptRoot
)

# Windows PowerShell 5.1 also needs .EXE to invoke reg.exe by its full path.
if (($env:PATHEXT -split ';') -notcontains '.EXE') {
    $env:PATHEXT = '.EXE;' + $env:PATHEXT
}

$script:SunshineRemovedCount = 0

# Remove only verbs whose command still belongs to this installation. The
# other Sunshine installation may have overwritten the same per-user verbs.
function Remove-OwnedSunshineMenus {
    param(
        [Microsoft.Win32.RegistryKey]$ClassesRoot,
        [string]$ExpectedExe
    )
    $verbs = @(
        @{ Key = '*\shell\Sunshine.SendToClient'; Argument = '--send-to-client "%1"' },
        @{ Key = 'Directory\shell\Sunshine.ShareFolder'; Argument = '--quick-share-folder "%1"' },
        @{ Key = 'Directory\Background\shell\Sunshine.ShareFolder'; Argument = '--quick-share-folder "%V"' }
    )
    foreach ($verb in $verbs) {
        $command = $ClassesRoot.OpenSubKey($verb.Key + '\command')
        if ($null -eq $command) { continue }
        try { $value = $command.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        finally { $command.Dispose() }
        if ($value -isnot [string]) { continue }
        $expected = '"' + $ExpectedExe + '" ' + $verb.Argument
        if ([string]::Equals($value.Trim(), $expected, [StringComparison]::OrdinalIgnoreCase)) {
            $ClassesRoot.DeleteSubKeyTree($verb.Key, $false)
            $script:SunshineRemovedCount++
            Write-Output ('Removed owned shell menu: ' + $verb.Key)
        }
    }
}

function Invoke-RegistryHiveCommand {
    param([string]$Operation, [string]$Mount, [string]$HiveFile)
    # Windows PowerShell 5.1 can turn native stderr into a terminating error
    # under Stop, before the caller can inspect the process exit code.
    $ErrorActionPreference = 'Continue'
    $reg = Join-Path $env:SystemRoot 'System32\reg.exe'
    if ($Operation -eq 'load') {
        & $reg load "HKU\$Mount" $HiveFile 2>$null | Out-Null
    } else {
        & $reg unload "HKU\$Mount" 2>$null | Out-Null
    }
    return $LASTEXITCODE
}

if ($MyInvocation.InvocationName -eq '.') { return }

$ErrorActionPreference = 'Stop'
$expectedExe = [IO.Path]::Combine([IO.Path]::GetFullPath($InstallRoot), 'assets\gui\sunshine-gui.exe')
Write-Output ('Cleaning shell menus owned by: ' + $expectedExe)
$users = [Microsoft.Win32.Registry]::Users
$errorsFound = $false
$seen = @{}
$userSid = '^S-1-(?:5-21-(?:\d+-){3}\d+|12-1-(?:\d+-){3}\d+)$'
$classesSid = $userSid -replace '\$$', '_Classes$'

# HKU includes loaded Classes hives for other desktop users too.
foreach ($classesName in $users.GetSubKeyNames() | Where-Object { $_ -match $classesSid }) {
    $sid = $classesName.Substring(0, $classesName.Length - '_Classes'.Length)
    $seen[$sid] = $true
    $root = $null
    try {
        $root = $users.OpenSubKey($classesName, $true)
        if ($null -eq $root) { throw 'Cannot open user Classes registry hive' }
        Remove-OwnedSunshineMenus -ClassesRoot $root -ExpectedExe $expectedExe
    } catch {
        $errorsFound = $true
        Write-Warning "Could not clean loaded user hive ${sid}: $_"
    } finally { if ($null -ne $root) { $root.Dispose() } }
}

# Explorer verbs live in UsrClass.dat, not in the user's NTUSER.DAT hive.
# Mount unloaded Classes hives under unique names and always unload them.
$profileList = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList')
if ($null -eq $profileList) { Write-Warning 'Could not inspect Windows user profiles'; exit 1 }
try {
    foreach ($sid in $profileList.GetSubKeyNames() | Where-Object { $_ -match $userSid -and -not $seen.ContainsKey($_) }) {
        $root = $null
        $mounted = $false
        try {
            $profileKey = $profileList.OpenSubKey($sid)
            if ($null -eq $profileKey) { throw 'Cannot open user profile' }
            try { $profilePath = $profileKey.GetValue('ProfileImagePath') }
            finally { $profileKey.Dispose() }
            if ($profilePath -isnot [string] -or -not $profilePath) { continue }
            $hiveFile = Join-Path ([Environment]::ExpandEnvironmentVariables($profilePath)) 'AppData\Local\Microsoft\Windows\UsrClass.dat'
            if (-not (Test-Path -LiteralPath $hiveFile -PathType Leaf)) { continue }
            $mount = 'SunshineCleanup_' + [guid]::NewGuid().ToString('N')
            $exitCode = Invoke-RegistryHiveCommand -Operation load -Mount $mount -HiveFile $hiveFile
            if ($exitCode -ne 0) { throw "Could not load user Classes hive (exit $exitCode)" }
            $mounted = $true
            $root = $users.OpenSubKey($mount, $true)
            if ($null -eq $root) { throw 'Cannot open mounted user Classes hive' }
            Remove-OwnedSunshineMenus -ClassesRoot $root -ExpectedExe $expectedExe
        } catch {
            $errorsFound = $true
            Write-Warning "Could not clean user hive ${sid}: $_"
        } finally {
            if ($null -ne $root) { $root.Dispose() }
            if ($mounted) {
                try {
                    $exitCode = Invoke-RegistryHiveCommand -Operation unload -Mount $mount
                    if ($exitCode -ne 0) { throw "reg unload exited $exitCode" }
                } catch {
                    $errorsFound = $true
                    Write-Warning "Could not unload temporary user hive for ${sid}: $_"
                }
            }
        }
    }
} finally { $profileList.Dispose() }
Write-Output ("Removed {0} owned shell menu(s)." -f $script:SunshineRemovedCount)
if ($errorsFound) { Write-Warning 'Some user profiles could not be processed; see the warnings above.'; exit 1 }
