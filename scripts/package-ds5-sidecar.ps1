param(
    [string]$RuntimeDirectory = "build/ds5-sidecar-runtime",
    [string]$PackageDirectory = "build/ds5-sidecar-package",
    [string]$ManifestPath = "build/ds5-sidecar-package.json",
    [string]$ReleaseTag = "",
    [string]$Repository = "AlkaidLab/foundation-sunshine"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$workspaceRoot = [System.IO.Path]::GetFullPath($root).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
$workspacePrefix = $workspaceRoot + [System.IO.Path]::DirectorySeparatorChar

function Resolve-WorkspacePath([string]$Path) {
    $resolved = [System.IO.Path]::GetFullPath((Join-Path $root $Path))
    if (-not $resolved.StartsWith($workspacePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to use a DualSense package path outside $workspaceRoot`: $resolved"
    }
    return $resolved
}

$runtime = Resolve-WorkspacePath $RuntimeDirectory
$packageOutput = Resolve-WorkspacePath $PackageDirectory
$manifestOutput = Resolve-WorkspacePath $ManifestPath
$assetName = 'Sunshine.Ds5Sidecar.Windows-x64.zip'
$componentVersion = '1.1.0'
$protocolVersion = 1

if (-not (Test-Path -LiteralPath (Join-Path $runtime 'Sunshine.Ds5Sidecar.exe') -PathType Leaf)) {
    throw "DualSense sidecar runtime is incomplete: $runtime"
}

New-Item -ItemType Directory -Force -Path $packageOutput | Out-Null
$archive = Join-Path $packageOutput $assetName
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $runtime '*') -DestinationPath $archive -CompressionLevel Optimal

$sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $archive).Length
$downloadUrl = ''
if (-not [string]::IsNullOrWhiteSpace($ReleaseTag)) {
    $escapedTag = [System.Uri]::EscapeDataString($ReleaseTag)
    $downloadUrl = "https://github.com/$Repository/releases/download/$escapedTag/$assetName"
}

$manifestDirectory = Split-Path -Parent $manifestOutput
New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
[ordered]@{
    schema = 1
    component_version = $componentVersion
    protocol = $protocolVersion
    target = 'win-x64-self-contained'
    asset_name = $assetName
    download_url = $downloadUrl
    sha256 = $sha256
    size = $size
} | ConvertTo-Json | Set-Content -LiteralPath $manifestOutput -Encoding utf8

Write-Host "Packaged DualSense sidecar: $archive ($size bytes, SHA-256 $sha256)"
Write-Host "Wrote DualSense package manifest: $manifestOutput"
