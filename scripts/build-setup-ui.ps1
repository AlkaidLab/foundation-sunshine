# Build the installer's setup UI (foundation-setup-ui.exe) from the committed Panel submodule: the pages (vite, installer
# front end of foundation-ui) and the Tauri 2 window (cargo, MSVC), the build docs/installer-webui.md describes.
# The Panel resolves foundation-ui as a sibling checkout named ds5-ui-m3e, so the submodule is copied next to it.
# usage: build-setup-ui.ps1 -FoundationUi <ds5-ui-m3e checkout, dependencies installed> -Output <foundation-setup-ui.exe>

param(
    [Parameter(Mandatory)][string]$FoundationUi,
    [Parameter(Mandatory)][string]$Output
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ui = (Resolve-Path -LiteralPath $FoundationUi).Path
# A relative -Output means relative to the caller's directory: resolve it before Push-Location changes the location.
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$work = Join-Path (Split-Path $ui -Parent) 'panel-setup-ui'
if (Test-Path -LiteralPath $work) { Remove-Item -Recurse -Force -LiteralPath $work }
$panel = Join-Path $repo 'src_assets/common/sunshine-control-panel'
if (-not (Test-Path -LiteralPath (Join-Path $panel 'installer-ui/Cargo.toml'))) { throw 'The Panel submodule is not checked out' }
# Without the pages root the build silently falls back to a placeholder page: refuse that.
if (-not (Test-Path -LiteralPath (Join-Path $ui 'src/vue/installer/MInstaller.vue'))) { throw "foundation-ui at $ui has no installer pages (src/vue/installer/MInstaller.vue)" }
New-Item -ItemType Directory -Path $work | Out-Null
# robocopy exits 0-7 on success (1 = files copied) and 8+ on failure, so its exit code is checked here rather than
# by $PSNativeCommandUseErrorActionPreference, which would throw on 1.
$PSNativeCommandUseErrorActionPreference = $false
robocopy $panel $work /E /XD .git node_modules target dist-installer /XF .git /NFL /NDL /NJH /NJS /NP | Out-Null
$robocopyExit = $LASTEXITCODE
$PSNativeCommandUseErrorActionPreference = $true
if ($robocopyExit -ge 8) { throw "robocopy failed: $robocopyExit" }
$global:LASTEXITCODE = 0

Push-Location $work
try {
    npm ci --no-audit --no-fund
    $env:FOUNDATION_UI_DIR = $ui
    npm run build:installer-pages
    # The pages import the whole foundation-ui stylesheet, which includes the clock components and their four fonts;
    # the installer page has no clock. The pruner removes those rules and files before cargo embeds the pages. It refuses
    # to delete anything the remaining CSS, JavaScript or HTML still names.
    python (Join-Path $repo 'scripts/ci/prune-setup-ui.py') --dist (Join-Path $work 'dist-installer')
    # $PSNativeCommandUseErrorActionPreference is PowerShell 7.4+ only; 5.1
    # hosts ignore it, so a refused prune must be caught by its exit code.
    if ($LASTEXITCODE -ne 0) { throw "prune-setup-ui.py failed with exit code $LASTEXITCODE" }
    cargo build --release --locked --manifest-path installer-ui/Cargo.toml
    $built = Join-Path $work 'installer-ui/target/release/foundation-setup-ui.exe'
    if (-not (Test-Path -LiteralPath $built) -or (Get-Item -LiteralPath $built).Length -eq 0) { throw 'foundation-setup-ui.exe was not produced' }
    New-Item -ItemType Directory -Force -Path (Split-Path $Output -Parent) | Out-Null
    Copy-Item -LiteralPath $built -Destination $Output -Force
    # The setup UI is a linked PE like any other module: its COFF symbol table is not needed at run time and is not
    # part of the crash-symbol flow (the symbol zip is built from the Panel and the host modules). The two jobs and the
    # packaging step run this again; the tool leaves a signed file alone.
    python (Join-Path $repo 'tools/pe_symbols.py') $Output
    if ($LASTEXITCODE -ne 0) { throw "pe_symbols.py failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
Write-Output "setup UI built: $Output"
