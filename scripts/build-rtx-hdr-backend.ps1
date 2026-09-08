param(
  [Parameter(Mandatory = $false)]
  [string] $SdkRoot = $env:RTX_VIDEO_SDK_ROOT,
  [Parameter(Mandatory = $false)]
  [string] $Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
  throw "Pass -SdkRoot or set RTX_VIDEO_SDK_ROOT."
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$backendSource = Join-Path $sourceRoot "tools\rtx_hdr_backend"
$buildRoot = Join-Path $sourceRoot "build\rtx-hdr-backend"

cmake -S $backendSource -B $buildRoot -G "Visual Studio 17 2022" -A x64 `
  -DRTX_VIDEO_SDK_ROOT="$SdkRoot" `
  -DSUNSHINE_SOURCE_DIR="$sourceRoot"
if ($LASTEXITCODE -ne 0) { throw "RTX HDR backend configure failed." }

cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "RTX HDR backend build failed." }

$output = Join-Path $buildRoot "$Configuration\foundation_truehdr_backend.dll"
if (!(Test-Path -LiteralPath $output)) {
  throw "Backend output was not produced: $output"
}
Write-Output $output
