param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseTag,

    [string]$OutputDirectory = "build/ds5-component",

    [string]$ManifestOutput = ""
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$runtime = [System.IO.Path]::GetFullPath((Join-Path $root $RuntimeDirectory))
$output = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$ReleaseTag = $ReleaseTag.Trim()

if ([string]::IsNullOrWhiteSpace($ReleaseTag) -or $ReleaseTag -eq 'latest') {
    throw 'ReleaseTag must name a concrete GitHub Release'
}
if (-not (Test-Path -LiteralPath $runtime -PathType Container)) {
    throw "DualSense sidecar runtime directory does not exist: $runtime"
}

$runtimeMetadataPath = Join-Path $runtime 'runtime.json'
if (-not (Test-Path -LiteralPath $runtimeMetadataPath -PathType Leaf)) {
    throw "DualSense sidecar runtime metadata does not exist: $runtimeMetadataPath"
}

$runtimeMetadata = Get-Content -LiteralPath $runtimeMetadataPath -Raw | ConvertFrom-Json
if ($runtimeMetadata.protocol -ne 1 -or $runtimeMetadata.target -ne 'win-x64-self-contained') {
    throw 'DualSense sidecar runtime metadata is incompatible with the release manifest schema'
}
$componentVersion = [string]$runtimeMetadata.component_version
if ([string]::IsNullOrWhiteSpace($componentVersion) -or
    $componentVersion.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw 'DualSense sidecar runtime metadata contains an invalid component version'
}

$entrypoint = 'Sunshine.Ds5Sidecar.exe'
if (-not (Test-Path -LiteralPath (Join-Path $runtime $entrypoint) -PathType Leaf)) {
    throw "DualSense sidecar entrypoint does not exist: $entrypoint"
}
if (Test-Path -LiteralPath (Join-Path $runtime 'HIDMaestro.Core.dll') -PathType Leaf) {
    throw 'DualSense sidecar runtime must not contain HIDMaestro.Core.dll'
}

$files = @(Get-ChildItem -LiteralPath $runtime -File | Sort-Object Name)
if ($files.Count -eq 0) {
    throw 'DualSense sidecar runtime has no files to package'
}
foreach ($file in $files) {
    if ($file.Name -match '[\\/]') {
        throw "DualSense sidecar runtime contains an invalid file name: $($file.Name)"
    }
}

$payloadFiles = @(
    foreach ($file in $files) {
        [ordered]@{
            path = $file.Name
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            size = [int64]$file.Length
        }
    }
)

$payload = [ordered]@{
    schema = 1
    component_version = $componentVersion
    protocol = [int]$runtimeMetadata.protocol
    rid = 'win-x64'
    self_contained = $true
    entrypoint = $entrypoint
    files = $payloadFiles
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
if ([string]::IsNullOrWhiteSpace($ManifestOutput)) {
    $ManifestOutput = Join-Path $output 'dualsense.json'
}
else {
    $ManifestOutput = [System.IO.Path]::GetFullPath((Join-Path $root $ManifestOutput))
}

$assetName = "Sunshine.Ds5Sidecar.$componentVersion.win-x64.zip"
$archivePath = Join-Path $output $assetName
$payloadManifestPath = Join-Path $output "payload-manifest-$PID.json"
$payloadJson = $payload | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($payloadManifestPath, $payloadJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

try {
    Add-Type -AssemblyName System.IO.Compression
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }

    $archive = [System.IO.Compression.ZipFile]::Open($archivePath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $archiveTimestamp = [DateTimeOffset]::new(2020, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $payloadEntry = $archive.CreateEntry('payload-manifest.json', [System.IO.Compression.CompressionLevel]::Optimal)
        $payloadEntry.LastWriteTime = $archiveTimestamp
        $payloadEntryStream = $payloadEntry.Open()
        try {
            $payloadBytes = [System.IO.File]::ReadAllBytes($payloadManifestPath)
            $payloadEntryStream.Write($payloadBytes, 0, $payloadBytes.Length)
        }
        finally {
            $payloadEntryStream.Dispose()
        }

        foreach ($file in $files) {
            $entry = $archive.CreateEntry($file.Name, [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $archiveTimestamp
            $entryStream = $entry.Open()
            $sourceStream = [System.IO.File]::OpenRead($file.FullName)
            try {
                $sourceStream.CopyTo($entryStream)
            }
            finally {
                $sourceStream.Dispose()
                $entryStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    Remove-Item -LiteralPath $payloadManifestPath -Force -ErrorAction SilentlyContinue
}

$archiveInfo = Get-Item -LiteralPath $archivePath
$payloadManifestSize = [int64][System.Text.UTF8Encoding]::new($false).GetByteCount($payloadJson + [Environment]::NewLine)
$expandedSize = [int64](($payloadFiles | Measure-Object -Property size -Sum).Sum) + $payloadManifestSize
$encodedTag = [System.Uri]::EscapeDataString($ReleaseTag)
$manifest = [ordered]@{
    schema = 1
    component = 'sunshine-dualsense'
    component_version = $componentVersion
    architecture = 'x86_64'
    sidecar_protocol = [int]$runtimeMetadata.protocol
    sunshine_version = $ReleaseTag
    sidecar = [ordered]@{
        url = "https://github.com/AlkaidLab/foundation-sunshine/releases/download/$encodedTag/$assetName"
        sha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        download_size = [int64]$archiveInfo.Length
        expanded_size = $expandedSize
        max_files = [int]($payloadFiles.Count + 1)
        entrypoint = $entrypoint
    }
    hidmaestro = [ordered]@{
        version = 'v1.6.1'
        url = 'https://github.com/hifihedgehog/HIDMaestro/releases/download/v1.6.1/HIDMaestro-v1.6.1.zip'
        sha256 = '00145c23d9838be6089389ce58b3fd2b6766fa9bc0f1f3c60a3c885361b53c34'
        download_size = [int64]118879222
        allow_files = @('HIDMaestro.Core.dll', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.txt')
    }
}

$manifestDirectory = Split-Path -Parent $ManifestOutput
New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
$manifestJson = $manifest | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($ManifestOutput, $manifestJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Host "Packaged DualSense sidecar release asset: $archivePath"
Write-Host "Generated DualSense component manifest: $ManifestOutput"
