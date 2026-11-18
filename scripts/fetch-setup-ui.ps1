# Fetch the installer's setup UI (foundation-setup-ui.exe) that the Panel repository built and published for the committed
# Panel submodule commit, instead of compiling it (scripts/build-setup-ui.ps1 takes about four minutes on a hosted runner).
#
# The pin is the `setupUi` entry of scripts/installer-inputs.json. The asset is used only if
#   - the pinned Panel commit is the committed Panel submodule and the pinned foundation-ui commit is scripts/ci/foundation-ui.sha
#     (the setup UI is built from both), and
#   - the download has the pinned sha256, and
#   - `gh attestation verify` proves that the Panel repository's publishing workflow built it from the pinned Panel commit on
#     its main branch
#     (https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations,
#      https://cli.github.com/manual/gh_attestation_verify).
# Exit codes: 0 the verified file is at -Output; 10 there is no matching asset (no pin, stale pin, or the download failed),
# so the caller builds from the submodule; anything else (a hash or attestation check that failed) is an error and must not
# be answered by building: a file that failed verification is never used.
# usage: fetch-setup-ui.ps1 -Output <foundation-setup-ui.exe>   (needs GH_TOKEN for gh)

param([Parameter(Mandatory)][string]$Output)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pins = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'installer-inputs.json') | ConvertFrom-Json
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)

function Skip([string]$Reason) {
    Write-Output "No prebuilt setup UI for this commit: $Reason"
    exit 10
}

$pin = $pins.setupUi
if (-not $pin) { Skip 'scripts/installer-inputs.json has no setupUi entry' }

$entry = git -C $repo ls-tree HEAD src_assets/common/sunshine-control-panel
if ($LASTEXITCODE -ne 0 -or $entry -notmatch '^160000 commit ([0-9a-f]{40})\s') { throw 'Unable to resolve the committed Panel submodule' }
if ($Matches[1] -ne $pin.commit) { Skip "the pin is for Panel commit $($pin.commit), the submodule is at $($Matches[1])" }
$foundationUi = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'ci/foundation-ui.sha')).Trim()
if ($foundationUi -ne $pin.foundationUi) { Skip "the pin is for foundation-ui $($pin.foundationUi), scripts/ci/foundation-ui.sha is $foundationUi" }

$temp = Join-Path ([IO.Path]::GetTempPath()) ("setup-ui-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $file = Join-Path $temp $pin.asset
    $url = "https://github.com/$($pin.repository)/releases/download/$($pin.tag)/$($pin.asset)"
    try {
        Invoke-WebRequest -Uri $url -OutFile $file -MaximumRetryCount 3 -RetryIntervalSec 3
    } catch {
        Skip "download of $url failed: $($_.Exception.Message)"
    }

    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $pin.sha256) { throw "sha256 mismatch for ${url}: expected $($pin.sha256), got $actual" }

    # The attestation names the repository, the workflow file and the source commit; all three are enforced.
    gh attestation verify $file --repo $pin.repository `
        --signer-workflow "$($pin.repository)/.github/workflows/setup-ui.yml" `
        --source-digest $pin.commit --source-ref refs/heads/main
    if ($LASTEXITCODE -ne 0) { throw "gh attestation verify failed for $($pin.asset) (exit $LASTEXITCODE)" }

    New-Item -ItemType Directory -Force -Path (Split-Path $Output -Parent) | Out-Null
    Copy-Item -LiteralPath $file -Destination $Output -Force
} finally {
    Remove-Item -Recurse -Force -LiteralPath $temp -ErrorAction SilentlyContinue
}
Write-Output "setup UI fetched and verified: $Output ($($pin.asset), Panel $($pin.commit))"
exit 0
