<#
 Keeps the unstripped binaries of a release for crash symbolisation (telemetry v1: module + offset -> function, file:line).

   archive-symbols.ps1 -Staging <inno_staging dir> -BuildInfo <build dir with version.h> -Out <symbols root> [-Panel <sunshine-gui.exe>]

 Run it BEFORE `strip`: it copies sunshine.exe, tools\sunshinesvc.exe, tools\sparxie-launch.exe and the panel's sunshine-gui.exe (+ any .pdb
 beside them) into <Out>\<version>\ and writes manifest.json (name, size, sha256, PE timestamp). <version> is PROJECT_VER with everything that is
 not [A-Za-z0-9._-] removed (the .杂鱼 suffix is dropped from the folder name only; the manifest keeps the full string). The alk-storage mover
 picks <Out> up from the Linux build host (see docs/infra/storage-tiers.md): the release recipe pulls the folder to /scratch/release-symbols/<version>/.
 The offline tool is scripts/release/symbolize.py.
#>
param(
  [Parameter(Mandatory)][string]$Staging,
  [Parameter(Mandatory)][string]$BuildInfo,
  [Parameter(Mandatory)][string]$Out,
  [string]$Panel = ''
)
$ErrorActionPreference = 'Stop'
$line = Select-String -Path (Join-Path $BuildInfo 'version.h') -Pattern '#define\s+PROJECT_VER\s+"([^"]*)"' | Select-Object -First 1
if (-not $line) { throw 'PROJECT_VER not found in version.h' }
$full = $line.Matches[0].Groups[1].Value
$name = ($full -replace '[^A-Za-z0-9._-]', '').Trim('.')
if (-not $name) { throw "no usable folder name from '$full'" }
$dest = Join-Path $Out $name
New-Item -ItemType Directory -Force $dest | Out-Null
$items = @(
  @{ src = (Join-Path $Staging 'sunshine.exe'); as = 'sunshine.exe' },
  @{ src = (Join-Path $Staging 'tools\sunshinesvc.exe'); as = 'sunshinesvc.exe' },
  @{ src = (Join-Path $Staging 'tools\sparxie-launch.exe'); as = 'sparxie-launch.exe' }
)
if ($Panel) { $items += @{ src = $Panel; as = 'sunshine-gui.exe' } }
$files = @()
foreach ($i in $items) {
  if (-not (Test-Path $i.src)) { Write-Output "SYMBOLS_MISSING $($i.as)"; continue }
  Copy-Item $i.src (Join-Path $dest $i.as) -Force
  $pdb = [IO.Path]::ChangeExtension($i.src, '.pdb')
  if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $dest ([IO.Path]::ChangeExtension($i.as, '.pdb'))) -Force }
  $f = Get-Item (Join-Path $dest $i.as)
  $files += [ordered]@{ name = $i.as; bytes = $f.Length; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower() }
}
$manifest = [ordered]@{ version = $full; folder = $name; archived_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); files = $files }
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $dest 'manifest.json')
Write-Output "SYMBOLS_ARCHIVED $dest $($files.Count) files"
exit $(if ($files.Count -ge 3) { 0 } else { 6 })
