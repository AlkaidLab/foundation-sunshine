param(
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Alias('MaxMbPerSession')][int]$ChunkMb = 128,
    [int]$MaxTotalMb = 2048,
    [ValidateSet('sonic','dolby')][string]$Backend = 'dolby'
)
# Run elevated. Explicitly started foreground supervisor, NOT a boot service.
# Stop: create OutputRoot/STOP, or Ctrl+C. It stops the child before returning.
$ErrorActionPreference = 'Stop'
if ($ChunkMb -lt 1 -or $MaxTotalMb -lt 1) { throw 'ChunkMb/MaxTotalMb must be positive' }
if (Test-Path -LiteralPath $OutputRoot) { throw 'OutputRoot must be a NEW directory' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$deployment = Join-Path $env:ProgramData 'SpatialObjectSpike'
New-Item -ItemType Directory -Path $deployment -Force | Out-Null
$deployed = Join-Path $deployment 'engine_tap_v2.dll'
$source = Join-Path $PSScriptRoot 'engine_tap.dll'
if ($Backend -eq 'dolby') { $source = Join-Path $PSScriptRoot 'stateful_tap.dll'; $deployed = Join-Path $deployment 'engine_tap_stateful_v1.dll' }
if (-not (Test-Path -LiteralPath $deployed)) { Copy-Item -LiteralPath $source -Destination $deployed }
if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $deployed).Hash) { throw 'Pinned DLL version mismatch; restart the audio engine normally before upgrading' }
$marker = Join-Path $OutputRoot 'STOP'
$child = $null
$session = 0
try {
    while (-not (Test-Path -LiteralPath $marker)) {
        $engines = @(Get-Process audiodg -ErrorAction SilentlyContinue)
        if ($engines.Count -ne 1) { Start-Sleep -Seconds 1; continue }
        $total = (Get-ChildItem -LiteralPath $OutputRoot -Filter '*.bin' -Recurse | Measure-Object Length -Sum).Sum
        if ($total -ge $MaxTotalMb * 1MB) { 'TOTAL_QUOTA_REACHED' | Add-Content (Join-Path $OutputRoot 'supervisor.log'); break }
        ++$session
        $dir = Join-Path $OutputRoot ('session-{0:D4}-pid-{1}' -f $session,$engines[0].Id)
        $sessionMb = [Math]::Max(1,[Math]::Floor(($MaxTotalMb * 1MB - $total)/1MB))
        $child = Start-Process -FilePath (Join-Path $PSScriptRoot 'collector.exe') -ArgumentList @('--output',"`"$dir`"",'--max-mb',"$sessionMb",'--chunk-mb',"$ChunkMb",'--backend',$Backend) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$dir.log" -RedirectStandardError "$dir.err"
        $retained = $child.Handle
        "Session=$session CollectorPID=$($child.Id) AudiodgPID=$($engines[0].Id)" | Add-Content (Join-Path $OutputRoot 'supervisor.log')
        while (-not $child.WaitForExit(500)) {
            if (Test-Path -LiteralPath $marker) { & (Join-Path $PSScriptRoot 'collector.exe') --stop $child.Id }
        }
        "Session=$session Exit=$($child.ExitCode)" | Add-Content (Join-Path $OutputRoot 'supervisor.log')
        # Fail closed for unknown versions, permissions and capture errors.
        # Only reattach automatically after a known engine process has exited.
        if ($child.ExitCode -ne 0 -and (Get-Process -Id $engines[0].Id -ErrorAction SilentlyContinue)) { break }
        $child = $null
        Start-Sleep -Seconds 1
    }
} finally {
    if ($child -and -not $child.HasExited) {
        & (Join-Path $PSScriptRoot 'collector.exe') --stop $child.Id
        $child.WaitForExit(10000) | Out-Null
    }
}
