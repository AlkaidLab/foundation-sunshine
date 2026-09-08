[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$LogPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet(
        'analysis-off',
        'd3d11-analysis',
        'd3d12-analysis',
        'd3d12-fused',
        'd3d12-encoder')]
    [string]$Scenario,

    [Parameter(Mandatory = $true)]
    [string]$Resolution,

    [Parameter(Mandatory = $true)]
    [int]$FrameRate,

    [Parameter(Mandatory = $true)]
    [double]$RefreshRate,

    [Parameter(Mandatory = $true)]
    [ValidateSet('vdd', 'wgc', 'ddapi')]
    [string]$CaptureBackend,

    [Parameter(Mandatory = $true)]
    [string]$EncoderBackend,

    [Parameter(Mandatory = $true)]
    [string]$Codec,

    [Parameter(Mandatory = $true)]
    [ValidateSet('sdr', 'pq', 'hlg')]
    [string]$HdrTransfer,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$RecordedFrames,

    [ValidateRange(1, 100)]
    [int]$Repeat = 1,

    [string]$OutputPath = (Join-Path $PWD 'video-pipeline-benchmark.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertFrom-MetricPayload {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Kind,

        [Parameter(Mandatory = $true)]
        [string]$Payload,

        [Parameter(Mandatory = $true)]
        [string]$Source
    )

    $metrics = [ordered]@{
        kind = $Kind
        source = $Source
    }
    foreach ($token in $Payload -split '\s+') {
        if ($token -match '^([^=]+)=(.+)$') {
            $name = $matches[1]
            $rawValue = $matches[2]
            [long]$integerValue = 0
            [double]$floatingValue = 0.0
            if ([long]::TryParse(
                $rawValue,
                [Globalization.NumberStyles]::Integer,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$integerValue)) {
                $metrics[$name] = $integerValue
            }
            elseif ([double]::TryParse(
                $rawValue,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$floatingValue)) {
                $metrics[$name] = $floatingValue
            }
            else {
                $metrics[$name] = $rawValue
            }
        }
    }
    return [pscustomobject]$metrics
}

$resolvedLogs = @(
    foreach ($path in $LogPath) {
        (Resolve-Path -LiteralPath $path).Path
    }
)

$summaries = @(
    foreach ($path in $resolvedLogs) {
        foreach ($line in Get-Content -LiteralPath $path) {
            if ($line -match '\[vram\] (gpu_metrics|cpu_metrics)\s+(.+)$') {
                ConvertFrom-MetricPayload -Kind $matches[1] -Payload $matches[2] -Source $path
            }
        }
    }
)

if ($summaries.Count -eq 0) {
    throw 'No [vram] gpu_metrics or cpu_metrics records were found. Run Sunshine with SUNSHINE_VRAM_TIMING=1.'
}

$videoControllers = @(
    Get-CimInstance Win32_VideoController |
        Select-Object Name, DriverVersion, AdapterRAM, PNPDeviceID
)
$operatingSystem = Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption, Version, BuildNumber

$report = [ordered]@{
    schema_version = 1
    captured_at_utc = [DateTime]::UtcNow.ToString('o')
    scenario = $Scenario
    resolution = $Resolution
    frame_rate = $FrameRate
    refresh_rate = $RefreshRate
    capture_backend = $CaptureBackend
    encoder_backend = $EncoderBackend
    codec = $Codec
    hdr_transfer = $HdrTransfer
    recorded_frames = $RecordedFrames
    repeat = $Repeat
    computer_name = $env:COMPUTERNAME
    operating_system = $operatingSystem
    video_controllers = $videoControllers
    source_logs = $resolvedLogs
    summaries = $summaries
}

$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "Wrote $($summaries.Count) telemetry summaries to $OutputPath"
