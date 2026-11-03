param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$PayloadDirectory,
    [string]$PolicyPath = 'tools/ds5-payload/policy.json'
)

# Replaces the upstream driver resources in an extracted payload with the signed
# package. The policy pins are the authority: a file that is not pinned, or whose
# hash disagrees with the pin, aborts the build. Nothing here signs or catalogs.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

$manifestPath = Resolve-RepoPath $Manifest
$payloadPath = Resolve-RepoPath $PayloadDirectory
$policyPath = Resolve-RepoPath $PolicyPath
foreach ($required in @($manifestPath, $policyPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "required file not found: $required" }
}

$document = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
$manifestDirectory = Split-Path -Parent $manifestPath

if ($document.schema -ne 1) { throw "driver package manifest has an unsupported schema: $($document.schema)" }
if ($document.arch -ne 'x64') { throw "driver package manifest has an unsupported arch: $($document.arch)" }
if ($policy.target -ne 'win-x64') { throw "payload policy targets $($policy.target); the driver package is x64" }
if (-not $document.files -or $document.files.Count -eq 0) { throw 'driver package manifest has no files' }

$pins = @{}
foreach ($pin in $policy.driver_package.files) { $pins[[string]$pin.name] = ([string]$pin.sha256).ToLowerInvariant() }
if ($pins.Count -eq 0) {
    throw 'payload policy has no driver_package pins; pin the signed package before importing it'
}
if ($pins.Count -ne $document.files.Count) {
    throw "payload policy pins $($pins.Count) resources but the manifest lists $($document.files.Count) files"
}

$entries = @{}
foreach ($file in $document.files) {
    $name = [string]$file.name
    $resource = [string]$file.resource
    if ([string]::IsNullOrWhiteSpace($resource)) { throw "driver file '$name' has no resource name" }
    if ([string]::IsNullOrWhiteSpace($name) -or [System.IO.Path]::IsPathRooted($name) -or
        $name.Contains('\') -or $name.Contains('/') -or $name -eq '.' -or $name -eq '..') {
        throw "driver file name is not a plain file name: '$name'"
    }
    if (-not $pins.ContainsKey($resource)) { throw "driver file '$name' is not pinned by the payload policy: $resource" }

    $path = Join-Path $manifestDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "driver file is missing: $path" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne ([string]$file.sha256).ToLowerInvariant()) {
        throw "driver file hash mismatch: $name"
    }
    if ($actual -ne $pins[$resource]) {
        throw "payload policy pin disagrees with the manifest for $resource"
    }
    $entries[$resource] = $path
}

foreach ($resource in $pins.Keys) {
    if (-not $entries.ContainsKey($resource)) {
        throw "payload policy pins a resource the manifest does not provide: $resource"
    }
}

$propsPath = Join-Path $payloadPath 'resources.props'
if (-not (Test-Path -LiteralPath $propsPath)) { throw "payload resources.props is missing: $propsPath" }
$props = [xml](Get-Content -LiteralPath $propsPath -Raw)
$replaced = 0
foreach ($node in @($props.Project.ItemGroup.EmbeddedResource)) {
    $logical = [string]$node.LogicalName
    if ($logical -and $entries.ContainsKey($logical)) {
        $node.ParentNode.RemoveChild($node) | Out-Null
        $replaced++
    }
}

$index = 0
foreach ($resource in ($entries.Keys | Sort-Object)) {
    $target = Join-Path $payloadPath ('package{0:D4}.bin' -f $index++)
    Copy-Item -LiteralPath $entries[$resource] -Destination $target -Force
    $node = $props.CreateElement('EmbeddedResource')
    $node.SetAttribute('Include', $target)
    $logical = $props.CreateElement('LogicalName')
    $logical.InnerText = $resource
    $node.AppendChild($logical) | Out-Null
    $props.Project.ItemGroup.AppendChild($node) | Out-Null
}
$props.Save($propsPath)

Write-Host ("Imported signed driver package {0}: {1} files, {2} upstream entries replaced" -f $document.driver_version, $entries.Count, $replaced)
