param(
    [string]$OutputDirectory = "build/ds5-sidecar-runtime",
    [string]$PackageCache = "build/ds5-sidecar-cache",
    [string]$ReleaseTag = "",
    [string]$Repository = "AlkaidLab/foundation-sunshine"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pin = Get-Content (Join-Path $root 'tools/hidmaestro-core-patch/upstream.json') -Raw | ConvertFrom-Json
$version = $pin.version
$expectedSha256 = $pin.release_sha256
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build'))
$cache = [System.IO.Path]::GetFullPath((Join-Path $root $PackageCache))
$output = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$buildPrefix = $buildRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
foreach ($target in @($cache, $output)) {
    if (-not $target.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to use a DualSense build path outside $buildRoot`: $target"
    }
}
if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
$dotnetSdkError = 'The .NET 10 SDK is required to publish the DualSense sidecar. Install Microsoft.DotNet.SDK.10 and retry.'
$dotnetCommand = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue
if ($null -ne $dotnetCommand) {
    $dotnet = $dotnetCommand.Source
}
else {
    $dotnet = Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'
    if (-not (Test-Path -LiteralPath $dotnet)) {
        throw $dotnetSdkError
    }
}

try {
    $installedSdks = @(& $dotnet --list-sdks 2>&1)
    $dotnetSdkExitCode = $LASTEXITCODE
}
catch {
    throw $dotnetSdkError
}
if (
    $dotnetSdkExitCode -ne 0 -or
    -not ($installedSdks | Where-Object { [string]$_ -match '^\s*10\.' })
) {
    throw $dotnetSdkError
}

# Rebuild only managed Core code, preserving the upstream ABI and payload.
& (Join-Path $PSScriptRoot 'build-hidmaestro-core-patch.ps1') -PackageCache $PackageCache -DotnetPath $dotnet
if ($LASTEXITCODE -ne 0) { throw 'HIDMaestro Core patch build failed' }
$core = Join-Path $buildRoot 'hidmaestro-core-patch/HIDMaestro.Core.dll'

& $dotnet publish (Join-Path $root 'tools/sunshine-ds5-sidecar/Sunshine.Ds5Sidecar.csproj') `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    --output $output `
    -p:HIDMaestroCorePath=$core `
    -p:DebugType=None `
    -p:DebugSymbols=false
if ($LASTEXITCODE -ne 0) {
    throw "DualSense sidecar publish failed with exit code $LASTEXITCODE"
}

# The optional component is a complete unit: runtime, patched Core and notices.
foreach ($name in @('LICENSE.HIDMaestro', 'THIRD-PARTY-NOTICES.txt')) {
    Copy-Item -LiteralPath (Join-Path $buildRoot "hidmaestro-core-patch/$name") -Destination $output -Force
}
$runtimeMetadata = @{
    component_version = '1.4.1'
    protocol = 1
    target = 'win-x64-self-contained'
    hidmaestro_build_version = $version
    hidmaestro_build_sha256 = $expectedSha256.ToLowerInvariant()
} | ConvertTo-Json
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    (Join-Path $output 'runtime.json'),
    $runtimeMetadata + [Environment]::NewLine,
    $utf8NoBom)

Write-Host "Published self-contained DualSense sidecar to $output"

& (Join-Path $PSScriptRoot 'package-ds5-sidecar.ps1') `
    -RuntimeDirectory $OutputDirectory `
    -ReleaseTag $ReleaseTag `
    -Repository $Repository
if ($LASTEXITCODE -ne 0) {
    throw "DualSense sidecar packaging failed with exit code $LASTEXITCODE"
}
