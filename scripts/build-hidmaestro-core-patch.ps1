param(
    [string]$PackageCache = 'build/ds5-sidecar-cache',
    [string]$OutputDirectory = 'build/hidmaestro-core-patch',
    [string]$DotnetPath = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ([string]::IsNullOrEmpty($DotnetPath)) {
    $command = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue
    $DotnetPath = if ($null -ne $command) { $command.Source } else { Join-Path $env:ProgramFiles 'dotnet/dotnet.exe' }
}
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'build'))
$buildPrefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$cache = [IO.Path]::GetFullPath((Join-Path $root $PackageCache))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
foreach ($target in @($cache, $output)) {
    if (-not $target.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Core patch build path must be inside $buildRoot`: $target"
    }
}
$patchRoot = Join-Path $root 'tools/hidmaestro-core-patch'
$pin = Get-Content -LiteralPath (Join-Path $patchRoot 'upstream.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $cache, $output | Out-Null

function Get-VerifiedArchive([string]$Url, [string]$Path, [string]$Digest) {
    if (-not (Test-Path -LiteralPath $Path) -or (Get-FileHash -LiteralPath $Path).Hash -ne $Digest) {
        Invoke-WebRequest -Uri $Url -OutFile $Path -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $Path).Hash -ne $Digest) {
        throw "Pinned Core input hash mismatch: $Path"
    }
}

$release = Join-Path $cache "HIDMaestro-$($pin.version).zip"
$sourceArchive = Join-Path $cache "source-$($pin.version).zip"
Get-VerifiedArchive "https://github.com/hifihedgehog/HIDMaestro/releases/download/$($pin.version)/HIDMaestro-$($pin.version).zip" $release $pin.release_sha256
Get-VerifiedArchive "https://codeload.github.com/hifihedgehog/HIDMaestro/zip/$($pin.commit)" $sourceArchive $pin.source_sha256

# Extract only verified upstream root files; no upstream build targets run.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$originalDirectory = Join-Path $cache 'original-core'
New-Item -ItemType Directory -Force -Path $originalDirectory | Out-Null
$archive = [IO.Compression.ZipFile]::OpenRead($release)
try {
    foreach ($name in @('HIDMaestro.Core.dll', 'LICENSE', 'THIRD-PARTY-NOTICES.txt')) {
        $entry = $archive.GetEntry($name)
        if ($null -eq $entry) { throw "Missing upstream file: $name" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $originalDirectory $name), $true)
    }
}
finally { $archive.Dispose() }
$originalCore = Join-Path $originalDirectory 'HIDMaestro.Core.dll'

$sourceDirectory = Join-Path $cache 'patched-source'
$payload = Join-Path $cache 'core-payload'
foreach ($directory in @($sourceDirectory, $payload)) {
    # Every recursive deletion is confined to checked, fixed build-cache paths.
    $resolved = [IO.Path]::GetFullPath($directory)
    if (-not $resolved.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw $resolved }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $resolved | Out-Null
}
Expand-Archive -LiteralPath $sourceArchive -DestinationPath $sourceDirectory
$source = Join-Path $sourceDirectory "HIDMaestro-$($pin.commit)"
$patch = Join-Path $patchRoot 'device-ownership.patch'
& git apply --check --unsafe-paths "--directory=$source" $patch
if ($LASTEXITCODE -ne 0) { throw 'Core patch no longer applies exactly to the pinned source' }
& git apply --unsafe-paths "--directory=$source" $patch
if ($LASTEXITCODE -ne 0) { throw 'Unable to apply Core patch' }

$payloadToolProject = Join-Path $patchRoot 'PayloadTool/PayloadTool.csproj'
& $DotnetPath build $payloadToolProject -c Release --nologo
if ($LASTEXITCODE -ne 0) { throw 'Core payload tool build failed' }
$payloadTool = Join-Path $patchRoot 'PayloadTool/bin/Release/net10.0-windows10.0.26100.0/PayloadTool.dll'
& $DotnetPath $payloadTool extract $originalCore $payload
if ($LASTEXITCODE -ne 0) { throw 'Core resource extraction failed' }
& $DotnetPath build (Join-Path $patchRoot 'HIDMaestro.Core.csproj') -c Release --nologo `
    "-p:UpstreamSource=$source" "-p:PayloadDirectory=$payload" "-p:OutputPath=$output/" `
    -p:IncludeSourceRevisionInInformationalVersion=false
if ($LASTEXITCODE -ne 0) { throw 'Patched Core build failed' }
$core = Join-Path $output 'HIDMaestro.Core.dll'
& $DotnetPath $payloadTool verify $originalCore $core
if ($LASTEXITCODE -ne 0) { throw 'Patched Core changed the upstream ABI or embedded payload' }
& $DotnetPath $payloadTool test $core (Join-Path $root 'tools/sunshine-ds5-sidecar/profiles/dualsense.json')
if ($LASTEXITCODE -ne 0) { throw 'Patched Core registry and device ownership regressions failed' }

Copy-Item -LiteralPath (Join-Path $originalDirectory 'LICENSE') -Destination (Join-Path $output 'LICENSE.HIDMaestro') -Force
Copy-Item -LiteralPath (Join-Path $originalDirectory 'THIRD-PARTY-NOTICES.txt') -Destination $output -Force
Write-Host "Built $($pin.patch): $core"
