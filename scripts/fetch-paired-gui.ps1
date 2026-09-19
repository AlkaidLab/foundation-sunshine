param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$RunId,
    [Parameter(Mandatory)][string]$Destination
)

$ErrorActionPreference = 'Stop'
$repository = 'qiin2333/sunshine-control-panel'
$entry = git ls-tree HEAD src_assets/common/sunshine-control-panel
if ($LASTEXITCODE -ne 0 -or $entry -notmatch '^160000 commit ([0-9a-f]{40})\s') {
    throw 'Unable to resolve the committed Panel submodule'
}
$expectedCommit = $Matches[1]
$runJson = gh api "repos/$repository/actions/runs/$RunId"
if ($LASTEXITCODE -ne 0) { throw 'Unable to read the Panel workflow run' }
$run = $runJson | ConvertFrom-Json
if ($run.conclusion -ne 'success' -or $run.head_sha -ne $expectedCommit -or
    $run.path -ne '.github/workflows/build.yml') {
    throw "Panel run must be a successful build.yml run at $expectedCommit"
}

# Keep the executable and its native plugin on one build boundary. Never use
# individual files from another release to fill gaps in this bundle.
if (Test-Path -LiteralPath $Destination) {
    throw 'Choose a fresh destination for the paired GUI bundle'
}
$download = Join-Path ([IO.Path]::GetTempPath()) ('paired-gui-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $download | Out-Null
gh run download $RunId --repo $repository --name sunshine-gui-windows-x64 --dir $download
if ($LASTEXITCODE -ne 0) { throw "Unable to download Panel artifact; diagnostic files: $download" }
$archive = Join-Path $download 'sunshine-gui-windows-x64.zip'
Expand-Archive -LiteralPath $archive -DestinationPath $Destination
foreach ($name in @('sunshine-gui.exe', 'alkaidlab-plugin-stylus.dll')) {
    $file = Get-Item -LiteralPath (Join-Path $Destination $name)
    if ($file.Length -eq 0) { throw "Empty required GUI file: $name" }
}
@{ repository = $repository; run_id = $RunId; commit = $expectedCommit } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'paired-build.json') -Encoding utf8
Write-Output "Paired GUI bundle verified at $expectedCommit in $Destination"
