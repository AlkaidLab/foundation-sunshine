param([string]$RunName = 'controlled01', [int]$Seconds = 20, [switch]$NoSource, [ValidateSet('sonic','dolby','dolby-raw','metadata')][string]$Backend = 'dolby', [int]$MaxMb = 512, [ValidateRange(1,1048576)][int]$ChunkMb = 128)
$ErrorActionPreference = 'Stop'
if ($RunName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Invalid run name' }
$out = Join-Path $PSScriptRoot $RunName
if (Test-Path -LiteralPath $out) { throw 'Output already exists' }
$outerLog = Join-Path $PSScriptRoot "$RunName-run.log"
$deployment = Join-Path $env:ProgramData 'SpatialObjectSpike'
New-Item -ItemType Directory -Path $deployment -Force | Out-Null
$sourceDll = Join-Path $PSScriptRoot 'engine_tap.dll'
$deployedDll = Join-Path $deployment 'engine_tap_v2.dll'
if ($Backend -eq 'dolby-raw') { $sourceDll = Join-Path $PSScriptRoot 'dolby_tap.dll'; $deployedDll = Join-Path $deployment 'engine_tap_dolby_v3.dll' }
if ($Backend -eq 'dolby') { $sourceDll = Join-Path $PSScriptRoot 'stateful_tap.dll'; $deployedDll = Join-Path $deployment 'engine_tap_stateful_v1.dll' }
if ($Backend -eq 'metadata') { $sourceDll = Join-Path $PSScriptRoot 'metadata_tap.dll'; $deployedDll = Join-Path $deployment 'engine_tap_metadata_v1.dll' }
if (-not (Test-Path -LiteralPath $deployedDll)) { Copy-Item -LiteralPath $sourceDll -Destination $deployedDll }
if ((Get-FileHash -LiteralPath $deployedDll).Hash -ne (Get-FileHash -LiteralPath $sourceDll).Hash) { throw 'Deployed DLL mismatch; do not overwrite a loaded DLL' }
Get-CimInstance Win32_Process -Filter "name='audiodg.exe'" | Invoke-CimMethod -MethodName GetOwner | Select-Object Domain,User,ReturnValue | Out-File -LiteralPath $outerLog
$warm = $null
if (-not $NoSource) {
    $warm = Start-Process -FilePath (Join-Path (Split-Path -Parent $PSScriptRoot) 'spatial_probe.exe') -ArgumentList '--emit --dev 1 --seconds 2 --objects 1 --variant 1' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot "$RunName-warm.log")
    Start-Sleep -Milliseconds 100
}
$collector = Start-Process -FilePath (Join-Path $PSScriptRoot 'collector.exe') -ArgumentList @('--output', "`"$out`"", '--seconds', "$Seconds", '--backend', $Backend, '--max-mb', "$MaxMb", '--chunk-mb', "$ChunkMb") -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot "$RunName-collector.log") -RedirectStandardError (Join-Path $PSScriptRoot "$RunName-error.log")
$collectorHandle = $collector.Handle
try {
    $limit = (Get-Date).AddSeconds(12)
    while (-not (Test-Path -LiteralPath (Join-Path $out 'ready.txt'))) {
        $collector.Refresh()
        if ($collector.HasExited) { throw "Collector exited $($collector.ExitCode)" }
        if ((Get-Date) -gt $limit) { throw 'Readiness timeout' }
        Start-Sleep -Milliseconds 100
    }
    if ($NoSource) {
        & (Join-Path $PSScriptRoot 'session_audit.exe') | Out-File -LiteralPath (Join-Path $out 'sessions-before.log') -Encoding utf8
        Get-Process audiodg | ForEach-Object { $_.Modules } | Where-Object { $_.ModuleName -match 'audioeng|dolby|dax|hrtf|engine_tap' } | Select-Object ModuleName,FileName,@{n='Version';e={$_.FileVersionInfo.FileVersion}} | Format-List | Out-File -LiteralPath (Join-Path $out 'engine-modules.log') -Encoding utf8
        Get-Process Nioh3 -ErrorAction SilentlyContinue | ForEach-Object {
            "GamePID=$($_.Id) Path=$($_.Path) CaptureDllLoaded=$([bool]($_.Modules.ModuleName -match 'engine_tap|spatial_tap'))"
        } | Add-Content -LiteralPath $outerLog
    }
    if (-not $NoSource) {
        $warm.WaitForExit()
        Start-Sleep -Milliseconds 200
        $source = Join-Path (Split-Path -Parent $PSScriptRoot) 'spatial_probe.exe'
        $sources = @()
        foreach ($variant in @(0, 1)) {
            $p = Start-Process -FilePath $source -ArgumentList "--emit --dev 1 --seconds 5 --objects 2 --variant $variant" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out "source-$variant.log")
            $retainedHandle = $p.Handle
            $sources += $p
            Start-Sleep -Milliseconds 150
            $modules = (Get-Process -Id $p.Id).Modules.ModuleName
            "SourcePID=$($p.Id) Variant=$variant CaptureDllLoaded=$([bool]($modules -match 'engine_tap|spatial_tap'))" | Add-Content -LiteralPath $outerLog
        }
        foreach ($p in $sources) { $p.WaitForExit(); "SourcePID=$($p.Id) Exit=$($p.ExitCode)" | Add-Content -LiteralPath $outerLog }
    }
    if ($NoSource) {
        while (-not $collector.WaitForExit(1000)) {
            $gamePids = @((Get-Process Nioh3 -ErrorAction SilentlyContinue).Id) -join ','
            $enginePids = @((Get-Process audiodg -ErrorAction SilentlyContinue).Id) -join ','
            "$(Get-Date -Format o) GamePIDs=$gamePids EnginePIDs=$enginePids" | Add-Content -LiteralPath (Join-Path $out 'lifecycle.log')
            if (Test-Path -LiteralPath (Join-Path $out 'STOP')) { & (Join-Path $PSScriptRoot 'collector.exe') --stop $collector.Id }
        }
        Get-Process Nioh3 -ErrorAction SilentlyContinue | ForEach-Object {
            "GameAfterPID=$($_.Id) Path=$($_.Path) CaptureDllLoaded=$([bool]($_.Modules.ModuleName -match 'engine_tap|spatial_tap|metadata_tap'))"
        } | Add-Content -LiteralPath $outerLog
    } else { $collector.WaitForExit() }
    if ($NoSource) { & (Join-Path $PSScriptRoot 'session_audit.exe') | Out-File -LiteralPath (Join-Path $out 'sessions-after.log') -Encoding utf8 }
    "CollectorPID=$($collector.Id) Exit=$($collector.ExitCode)" | Add-Content -LiteralPath $outerLog
} finally {
    $collector.Refresh()
    if (-not $collector.HasExited) {
        & (Join-Path $PSScriptRoot 'collector.exe') --stop $collector.Id
        $collector.WaitForExit()
    }
}
