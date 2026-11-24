# Fetch the pinned binary inputs of the Windows installer into a fresh directory and verify each one:
#   gui      the Panel bundle (sunshine-gui.exe + its native plugin), a release asset of the Panel tag pinned in
#            installer-inputs.json; the pinned commit must be the committed Panel submodule, so the GUI and the setup UI
#            built from that submodule (build-setup-ui.ps1) are one Panel build boundary
# A changed hash fails the step: bump installer-inputs.json deliberately, never fetch "latest".
# The WebView2 bootstrapper is not an input: the installer worker downloads it from Microsoft when the Runtime is missing
# (tools/installer_webview2.h), so nothing of Microsoft's is pinned or shipped.
# usage: fetch-installer-inputs.ps1 -Destination <new dir>   -> <dir>/gui/

param([Parameter(Mandatory)][string]$Destination)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$pins = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'installer-inputs.json') | ConvertFrom-Json

function Get-Pinned([string]$Url, [string]$Path, [string]$Sha256) {
    Invoke-WebRequest -Uri $Url -OutFile $Path -MaximumRetryCount 4 -RetryIntervalSec 5
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "sha256 mismatch for ${Url}: expected $Sha256, got $actual" }
}

if (Test-Path -LiteralPath $Destination) { throw 'Choose a fresh destination' }
New-Item -ItemType Directory -Path $Destination | Out-Null

# The Panel pin and the committed Panel submodule must agree.
$entry = git -C (Join-Path $PSScriptRoot '..') ls-tree HEAD src_assets/common/sunshine-control-panel
if ($LASTEXITCODE -ne 0 -or $entry -notmatch '^160000 commit ([0-9a-f]{40})\s') { throw 'Unable to resolve the committed Panel submodule' }
if ($Matches[1] -ne $pins.gui.commit) {
    throw "Panel submodule is at $($Matches[1]) but scripts/installer-inputs.json pins the GUI of $($pins.gui.commit) ($($pins.gui.tag)); bump the pin together with the submodule"
}

$guiZip = Join-Path $Destination $pins.gui.asset
Get-Pinned "https://github.com/$($pins.gui.repository)/releases/download/$($pins.gui.tag)/$($pins.gui.asset)" $guiZip $pins.gui.sha256
Expand-Archive -LiteralPath $guiZip -DestinationPath (Join-Path $Destination 'gui')
Remove-Item -LiteralPath $guiZip
foreach ($name in @('sunshine-gui.exe', 'alkaidlab-plugin-stylus.dll')) {
    $file = Get-Item -LiteralPath (Join-Path $Destination "gui/$name")
    if ($file.Length -eq 0) { throw "Empty required GUI file: $name" }
}

Write-Output "GUI $($pins.gui.tag) ($($pins.gui.commit)) verified in $Destination"
