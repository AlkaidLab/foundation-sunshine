# Exercises the production removal function only under an isolated HKCU test
# key. It never creates or removes actual Explorer verbs.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '../../src_assets/windows/misc/shell/remove-shell-menus.ps1'
. $script -InstallRoot 'C:\Unused'

$fixture = 'Software\SunshineShellCleanupTest_' + [guid]::NewGuid().ToString('N')
$root = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($fixture)
$fileVerb = 'Software\Classes\*\shell\Sunshine.SendToClient'
$folderVerb = 'Software\Classes\Directory\shell\Sunshine.ShareFolder'
$backgroundVerb = 'Software\Classes\Directory\Background\shell\Sunshine.ShareFolder'
$exe = 'C:\Program Files\Sunshine\assets\gui\sunshine-gui.exe'

function Set-FixtureCommand([string]$Verb, [string]$Value) {
    $command = $root.CreateSubKey($Verb + '\command')
    try { $command.SetValue('', $Value) } finally { $command.Dispose() }
}
function Assert-FixtureKey([string]$Verb, [bool]$Exists) {
    $key = $root.OpenSubKey($Verb)
    try { if (($null -ne $key) -ne $Exists) { throw "Unexpected fixture state for $Verb" } }
    finally { if ($null -ne $key) { $key.Dispose() } }
}

try {
    Set-FixtureCommand $fileVerb ('"' + $exe + '" --send-to-client "%1"')
    Set-FixtureCommand $folderVerb '"D:\Portable\assets\gui\sunshine-gui.exe" --quick-share-folder "%1"'
    Set-FixtureCommand $backgroundVerb ('"' + $exe + '" --quick-share-folder "%V"')
    Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $exe
    Assert-FixtureKey $fileVerb $false
    Assert-FixtureKey $folderVerb $true
    Assert-FixtureKey $backgroundVerb $false

    # Unknown or modified commands, including another executable under the
    # same directory, must not be deleted. A repeated call is harmless.
    Set-FixtureCommand $fileVerb ('"' + $exe + '" --other-option "%1"')
    Set-FixtureCommand $backgroundVerb '"C:\Program Files\Sunshine-other\assets\gui\sunshine-gui.exe" --quick-share-folder "%V"'
    Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $exe
    Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $exe
    Assert-FixtureKey $fileVerb $true
    Assert-FixtureKey $folderVerb $true
    Assert-FixtureKey $backgroundVerb $true

    # All three owned verbs are removed independently; case-only differences
    # in a Windows executable path do not change ownership.
    Set-FixtureCommand $fileVerb ('"' + $exe.ToUpperInvariant() + '" --send-to-client "%1"')
    Set-FixtureCommand $folderVerb ('"' + $exe + '" --quick-share-folder "%1"')
    Set-FixtureCommand $backgroundVerb ('"' + $exe + '" --quick-share-folder "%V"')
    Remove-OwnedSunshineMenus -UserRoot $root -ExpectedExe $exe
    Assert-FixtureKey $fileVerb $false
    Assert-FixtureKey $folderVerb $false
    Assert-FixtureKey $backgroundVerb $false
    Write-Output 'PASS: owned menus removed; other and modified commands preserved; repeated cleanup is safe.'
} finally {
    $root.Dispose()
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($fixture, $false)
}
