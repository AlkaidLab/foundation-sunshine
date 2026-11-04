param(
    [Parameter(Mandatory = $true)]
    [string]$Artifact,
    [string]$Policy = 'tools/ds5-payload/policy.json',
    [string]$Report = 'build/ds5-payload-audit.json'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

$artifactPath = Resolve-RepoPath $Artifact
$policyPath = Resolve-RepoPath $Policy
$reportPath = Resolve-RepoPath $Report

if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
    throw "DualSense payload artifact is not a file: $artifactPath"
}
if (-not (Test-Path -LiteralPath $policyPath -PathType Leaf)) {
    throw "DualSense payload policy not found: $policyPath"
}

$dotnetCommand = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue
$dotnet = if ($null -ne $dotnetCommand) { $dotnetCommand.Source } else { Join-Path $env:ProgramFiles 'dotnet\dotnet.exe' }
if (-not (Test-Path -LiteralPath $dotnet)) {
    throw 'The .NET SDK is required to run the DualSense payload audit.'
}

$project = Join-Path $root 'tools/ds5-payload/Ds5PayloadAudit/Ds5PayloadAudit.csproj'
& $dotnet build $project -c Release --nologo
if ($LASTEXITCODE -ne 0) { throw 'DualSense payload audit tool build failed' }

$tool = Join-Path $root 'tools/ds5-payload/Ds5PayloadAudit/bin/Release/net10.0/Ds5PayloadAudit.dll'
$auditArguments = @($artifactPath, '--policy', $policyPath, '--report', $reportPath)

& $dotnet $tool @auditArguments
$exitCode = $LASTEXITCODE

if (Test-Path -LiteralPath $reportPath) {
    Write-Host "DualSense payload audit report: $reportPath"
}
exit $exitCode
