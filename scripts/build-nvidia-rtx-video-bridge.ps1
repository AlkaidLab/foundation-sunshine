param(
  [Parameter(Mandatory = $false)]
  [string] $SdkRoot = $env:RTX_VIDEO_SDK_ROOT,
  [Parameter(Mandatory = $false)]
  [string] $Configuration = "Release",
  [Parameter(Mandatory = $false)]
  [string] $NgxApplicationId = $env:RTX_VIDEO_NGX_APPLICATION_ID,
  [Parameter(Mandatory = $false)]
  [string] $BuildDirectory = 'build'
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
  [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
  [System.IO.Path]::GetFullPath((Join-Path $sourceRoot $BuildDirectory))
}
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
  throw "Pass -SdkRoot or set RTX_VIDEO_SDK_ROOT."
}
if ([string]::IsNullOrWhiteSpace($NgxApplicationId)) {
  $NgxApplicationId = '0'
}
$parsedApplicationId = [UInt64]0
if ($NgxApplicationId -notmatch '^(0|[1-9][0-9]*)$' -or ![UInt64]::TryParse($NgxApplicationId, [ref]$parsedApplicationId)) {
  throw "NgxApplicationId must be an unsigned decimal integer."
}
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$bridgeSource = Join-Path $sourceRoot "src\platform\windows\hdr_enhanced\nvidia_rtx_video\bridge"
$buildRoot = Join-Path $outputRoot "hdr_enhanced\nvidia_rtx_video_bridge"

cmake -S $bridgeSource -B $buildRoot -G "Visual Studio 17 2022" -A x64 `
  -DRTX_VIDEO_SDK_ROOT="$SdkRoot" `
  -DRTX_VIDEO_NGX_APPLICATION_ID="$parsedApplicationId" `
  -DSUNSHINE_SOURCE_DIR="$sourceRoot"
if ($LASTEXITCODE -ne 0) { throw "RTX Video bridge configure failed." }

cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "RTX Video bridge build failed." }

$output = Join-Path $buildRoot "$Configuration\foundation_rtx_video_bridge.dll"
if (!(Test-Path -LiteralPath $output)) {
  throw "Bridge output was not produced: $output"
}

# 只向主安装包提供桥接组件及其可信哈希，不打包 NVIDIA 运行库。
$runtime = Join-Path $buildRoot "$Configuration\nvngx_truehdr.dll"
if (!(Test-Path -LiteralPath $runtime -PathType Leaf)) {
  throw "The matched NVIDIA runtime was not produced."
}
$bridgeHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
$runtimeHash = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
$version = $bridgeHash.Substring(0, 16) + '-' + $runtimeHash.Substring(0, 16)
$versions = [ordered]@{}
$versions[$version] = [ordered]@{
  'foundation_rtx_video_bridge.dll' = $bridgeHash
  'nvngx_truehdr.dll' = $runtimeHash
}
$catalog = [ordered]@{
  schema_version = 1
  components = [ordered]@{ 'alkaidlab.nvidia_rtx_video' = $versions }
}
$assets = Join-Path $outputRoot 'assets'
[System.IO.Directory]::CreateDirectory($assets) | Out-Null
$catalogPath = Join-Path $assets 'hdr-components.json'
[System.IO.File]::WriteAllText($catalogPath, ($catalog | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
$runtimeRoot = Join-Path $outputRoot 'tools\hdr_enhanced\nvidia_rtx_video'
[System.IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
Copy-Item -LiteralPath $output -Destination (Join-Path $runtimeRoot 'foundation_rtx_video_bridge.dll') -Force
Copy-Item -LiteralPath (Join-Path $bridgeSource '..\README.md') -Destination (Join-Path $runtimeRoot 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $bridgeSource 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $runtimeRoot 'RTX_VIDEO_THIRD_PARTY_NOTICES.md') -Force
Write-Output "Trusted component metadata: $catalogPath"
Write-Output $output
