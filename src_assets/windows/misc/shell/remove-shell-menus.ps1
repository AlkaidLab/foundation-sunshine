param(
    # When copied into an installation directory, running the script without
    # arguments targets that directory. Uninstallers pass their root explicitly.
    [string]$InstallRoot = $PSScriptRoot
)

$script:SunshineRemovedCount = 0

# Remove only verbs whose command still belongs to this installation. The
# other Sunshine installation may have overwritten the same per-user verbs.
function Remove-OwnedSunshineMenus {
    param(
        [Microsoft.Win32.RegistryKey]$UserRoot,
        [string]$ExpectedExe
    )
    $verbs = @(
        @{ Key = 'Software\Classes\*\shell\Sunshine.SendToClient'; Argument = '--send-to-client "%1"' },
        @{ Key = 'Software\Classes\Directory\shell\Sunshine.ShareFolder'; Argument = '--quick-share-folder "%1"' },
        @{ Key = 'Software\Classes\Directory\Background\shell\Sunshine.ShareFolder'; Argument = '--quick-share-folder "%V"' }
    )
    foreach ($verb in $verbs) {
        $command = $UserRoot.OpenSubKey($verb.Key + '\command')
        if ($null -eq $command) { continue }
        try { $value = $command.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        finally { $command.Dispose() }
        if ($value -isnot [string]) { continue }
        $expected = '"' + $ExpectedExe + '" ' + $verb.Argument
        if ([string]::Equals($value.Trim(), $expected, [StringComparison]::OrdinalIgnoreCase)) {
            $UserRoot.DeleteSubKeyTree($verb.Key, $false)
            $script:SunshineRemovedCount++
            Write-Output ('Removed owned shell menu: ' + $verb.Key)
        }
    }
}

if ($MyInvocation.InvocationName -eq '.') { return }

$ErrorActionPreference = 'Stop'
$expectedExe = [IO.Path]::Combine([IO.Path]::GetFullPath($InstallRoot), 'assets\gui\sunshine-gui.exe')
Write-Output ('Cleaning shell menus owned by: ' + $expectedExe)
$users = [Microsoft.Win32.Registry]::Users
$errorsFound = $false
$seen = @{}
$userSid = '^S-1-(?:5-21-(?:\d+-){3}\d+|12-1-(?:\d+-){3}\d+)$'

# HKU includes all currently loaded user hives, including a different desktop
# user when the uninstaller was elevated with another administrator account.
foreach ($sid in $users.GetSubKeyNames() | Where-Object { $_ -match $userSid }) {
    $seen[$sid] = $true
    $root = $null
    try {
        $root = $users.OpenSubKey($sid, $true)
        if ($null -eq $root) { throw 'Cannot open user registry hive' }
        Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $expectedExe
    } catch {
        $errorsFound = $true
        Write-Warning "Could not clean loaded user hive ${sid}: $_"
    } finally { if ($null -ne $root) { $root.Dispose() } }
}

# Unloaded profiles also retain Explorer verbs. Mount each NTUSER.DAT under a
# unique temporary HKU name, close every handle and always unload it afterwards.
$profileList = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList')
if ($null -eq $profileList) { Write-Warning 'Could not inspect Windows user profiles'; exit 1 }
try {
    foreach ($sid in $profileList.GetSubKeyNames() | Where-Object { $_ -match $userSid -and -not $seen.ContainsKey($_) }) {
        $profile = $profileList.OpenSubKey($sid)
        if ($null -eq $profile) { continue }
        try { $profilePath = $profile.GetValue('ProfileImagePath') }
        finally { $profile.Dispose() }
        if ($profilePath -isnot [string] -or -not $profilePath) { continue }
        $hiveFile = Join-Path ([Environment]::ExpandEnvironmentVariables($profilePath)) 'NTUSER.DAT'
        if (-not (Test-Path -LiteralPath $hiveFile -PathType Leaf)) { continue }
        $mount = 'SunshineCleanup_' + [guid]::NewGuid().ToString('N')
        $reg = Join-Path $env:SystemRoot 'System32\reg.exe'
        & $reg load "HKU\$mount" $hiveFile 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $errorsFound = $true
            Write-Warning "Could not load user hive ${sid}; its shell menus may remain"
            continue
        }
        $root = $null
        try {
            $root = $users.OpenSubKey($mount, $true)
            if ($null -eq $root) { throw 'Cannot open mounted user registry hive' }
            Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $expectedExe
        } catch {
            $errorsFound = $true
            Write-Warning "Could not clean user hive ${sid}: $_"
        } finally {
            if ($null -ne $root) { $root.Dispose() }
            & $reg unload "HKU\$mount" 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) {
                $errorsFound = $true
                Write-Warning "Could not unload temporary user hive for ${sid}"
            }
        }
    }
} finally { $profileList.Dispose() }
Write-Output ("Removed {0} owned shell menu(s)." -f $script:SunshineRemovedCount)
if ($errorsFound) { Write-Warning 'Some user profiles could not be processed; see the warnings above.'; exit 1 }
