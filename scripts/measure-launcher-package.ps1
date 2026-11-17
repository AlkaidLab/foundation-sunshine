param([Parameter(Mandatory = $true)][string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$projectDir = Join-Path $root 'tools/sparxie-launch'
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $output | Out-Null
$rows = @()
# ms-dotnet-aot-size-20261117, Optimize for size or speed. Both arms use
# the same SDK, Release configuration and sources, changing only the hint.
foreach ($trial in 1..2) {
    $arms = if ($trial -eq 1) { @('default', 'size') } else { @('size', 'default') }
    foreach ($arm in $arms) {
        $dir = Join-Path $output "$arm-$trial"
        # Start every trial from an empty directory so a rerun cannot trip over an old archive.
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        $publish = Join-Path $dir 'publish'
        # Build from a copy without the project's own obj/bin: the SDK globs every .cs file under
        # the project directory, so generated files from an earlier build would be compiled twice.
        $src = Join-Path $dir 'src'
        New-Item -ItemType Directory -Force $src | Out-Null
        Get-ChildItem $projectDir -Force | Where-Object { $_.Name -notin @('obj', 'bin') } |
            Copy-Item -Destination $src -Recurse -Force
        $hint = if ($arm -eq 'size') { 'Size' } else { '' }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        & dotnet publish (Join-Path $src 'Sparxie.Launch.csproj') -c Release -r win-x64 --self-contained true `
            --output $publish --disable-build-servers --nologo `
            "-p:BaseIntermediateOutputPath=$dir/obj/" "-p:BaseOutputPath=$dir/bin/" `
            "-p:OptimizationPreference=$hint"
        if ($LASTEXITCODE -ne 0) { throw "publish failed: $arm trial $trial" }
        $watch.Stop()
        $buildMs = $watch.Elapsed.TotalMilliseconds
        $exe = Join-Path $publish 'sparxie-launch.exe'
        $zip = Join-Path $dir 'launcher.zip'
        $watch.Restart()
        $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
        try {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $exe,
                'sparxie-launch.exe', [IO.Compression.CompressionLevel]::SmallestSize) | Out-Null
        } finally { $archive.Dispose() }
        $watch.Stop()
        $packMs = $watch.Elapsed.TotalMilliseconds
        # No launch target: this exercises process/runtime startup and argument
        # validation only. It does not measure game launch or encoder first frame.
        $starts = @()
        foreach ($sample in 0..20) {
            $watch.Restart()
            $process = Start-Process -FilePath $exe -ArgumentList '--package-size-check' -PassThru -Wait
            $watch.Stop()
            if ($process.ExitCode -ne 2) { throw "unexpected argument-check exit: $($process.ExitCode)" }
            if ($sample -gt 0) { $starts += $watch.Elapsed.TotalMilliseconds }
        }
        $ordered = @($starts | Sort-Object)
        $rows += [ordered]@{
            arm = $arm; trial = $trial
            raw_bytes = (Get-Item $exe).Length; zip_bytes = (Get-Item $zip).Length
            publish_ms = $buildMs; zip_ms = $packMs
            startup_samples_ms = $starts
            startup_p50_ms = $ordered[9]; startup_p95_ms = $ordered[18]; startup_p99_ms = $ordered[19]
            sha256 = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
[ordered]@{
    schema = 1; sdk = (& dotnet --version); os = [Environment]::OSVersion.VersionString
    method = 'Alternating Release Native AOT builds; ZIP SmallestSize; 20 warm argument-validation process starts per arm/trial'
    rows = $rows
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'summary.json') -Encoding utf8
$rows | Select-Object arm, trial, raw_bytes, zip_bytes, publish_ms, zip_ms, startup_p50_ms | Format-Table
