param(
  [Parameter(Mandatory = $false)]
  [string] $SdkRoot = "",
  [Parameter(Mandatory = $false)]
  [ValidateSet('Release')]
  [string] $Configuration = "Release",
  [Parameter(Mandatory = $false)]
  [string] $NgxApplicationId = $env:RTX_VIDEO_NGX_APPLICATION_ID,
  [Parameter(Mandatory = $false)]
  [string] $BuildDirectory = 'build'
)

$ErrorActionPreference = "Stop"
# This optional standalone build never downloads SDK files.
Remove-Item Env:RTX_VIDEO_SDK_TOKEN -ErrorAction SilentlyContinue
Remove-Item Env:DRIVER_DOWNLOAD_TOKEN -ErrorAction SilentlyContinue
$sourceRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
  [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
  [System.IO.Path]::GetFullPath((Join-Path $sourceRoot $BuildDirectory))
}
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
  throw "Pass -SdkRoot. The main Sunshine build acquires the SDK automatically."
}
if ([string]::IsNullOrWhiteSpace($NgxApplicationId)) {
  $NgxApplicationId = '0'
}
$parsedApplicationId = [UInt64]0
if ($NgxApplicationId -notmatch '^(0|[1-9][0-9]*)$' -or ![UInt64]::TryParse($NgxApplicationId, [ref]$parsedApplicationId)) {
  throw "NgxApplicationId must be an unsigned decimal integer."
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$adapterSource = Join-Path $sourceRoot "src\platform\windows\hdr_enhanced\nvidia_rtx_video\adapter"
$buildRoot = Join-Path $outputRoot "hdr_enhanced\nvidia_rtx_video_adapter"

cmake -S $adapterSource -B $buildRoot -G "Visual Studio 17 2022" -A x64 `
  -DNVIDIA_RTX_VIDEO_SDK_DIR="$SdkRoot" `
  -DRTX_VIDEO_NGX_APPLICATION_ID="$parsedApplicationId" `
  -DSUNSHINE_SOURCE_DIR="$sourceRoot"
if ($LASTEXITCODE -ne 0) { throw "RTX Video adapter configure failed." }

cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "RTX Video adapter build failed." }

# 适配器由 Sunshine 受限加载；NVIDIA 运行库只供本地测试，不进入安装包。
$output = Join-Path $buildRoot "$Configuration\foundation_rtx_video_adapter.dll"
if (!(Test-Path -LiteralPath $output -PathType Leaf)) {
  throw "RTX Video adapter DLL was not produced."
}
Write-Output "Adapter DLL: $output"
Write-Output "Standalone adapter build complete. Normal Sunshine builds prepare the SDK through CMake automatically."
