param(
    [string]$ProbeExe = "",
    [int]$TimeoutMs = 5000,
    [string]$MatchSubstring = "VID_1ACE&PID_0002",
    [switch]$ManualInput,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ProbeExe {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        return (Resolve-Path $ExplicitPath).Path
    }

    $candidates = @(
        (Join-Path $PSScriptRoot "..\build\tests\vmouse_probe.exe"),
        (Join-Path $PSScriptRoot "..\build-test\tests\vmouse_probe.exe"),
        (Join-Path $PSScriptRoot "..\out\build\tests\vmouse_probe.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "找不到 vmouse_probe.exe，请使用 -ProbeExe 显式指定。"
}

function Get-VMousePnpInfo {
    $device = Get-PnpDevice -InstanceId 'ROOT\HIDCLASS\*' -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.FriendlyName -like '*Virtual Mouse*' -or $_.HardwareID -contains 'Root\ZakoVirtualMouse') -and
            $null -ne $_.FriendlyName -and
            $_.FriendlyName -ne ''
        } |
        Select-Object -First 1 Status, FriendlyName, Problem, InstanceId

    if ($null -eq $device) {
        return [pscustomobject]@{
            Installed  = $false
            Running    = $false
            StatusText = "未安装"
            Device     = $null
        }
    }

    $running = $device.Status -eq "OK" -and $device.Problem -eq 0
    $statusText = if ($running) {
        "$($device.FriendlyName) - 正常运行"
    }
    elseif ($device.Problem -eq 21) {
        "$($device.FriendlyName) - 需要重启"
    }
    else {
        "$($device.FriendlyName) - Status=$($device.Status), Problem=$($device.Problem)"
    }

    return [pscustomobject]@{
        Installed  = $true
        Running    = $running
        StatusText = $statusText
        Device     = $device
    }
}

function Invoke-Probe {
    param([string]$ExePath, [string[]]$Arguments)

    $lines = & $ExePath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $values = @{}

    foreach ($line in $lines) {
        if ($line -match '^([A-Z_]+)=(.*)$') {
            $values[$matches[1]] = $matches[2]
        }
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines    = $lines
        Values   = $values
    }
}

function Get-IntValue {
    param(
        [hashtable]$Table,
        [string]$Key
    )

    if ($Table.ContainsKey($Key)) {
        return [int]$Table[$Key]
    }

    return 0
}

$probePath = Resolve-ProbeExe -ExplicitPath $ProbeExe
$pnpInfo = Get-VMousePnpInfo

Write-Host "PnP 状态: $($pnpInfo.StatusText)"
if (-not $pnpInfo.Installed) {
    throw "未检测到 Root\ZakoVirtualMouse。请先安装驱动。"
}

$listResult = Invoke-Probe -ExePath $probePath -Arguments @(
    "--list-only",
    "--match-substring", $MatchSubstring
)

if (-not $Quiet) {
    $listResult.Lines | ForEach-Object { Write-Host $_ }
}

if ((Get-IntValue -Table $listResult.Values -Key "MATCHED_DEVICE_PRESENT") -ne 1) {
    throw "Raw Input 设备枚举中未发现匹配的虚拟鼠标。"
}

$probeArgs = @(
    "--timeout-ms", "$TimeoutMs",
    "--match-substring", $MatchSubstring,
    "--require-device",
    "--require-events"
)

if ($ManualInput) {
    Write-Host "请在接下来的 $TimeoutMs ms 内，通过 Sunshine/Moonlight 触发一次虚拟鼠标移动或点击。"
}
else {
    $probeArgs += "--send-test-sequence"
}

if ($Quiet) {
    $probeArgs += "--quiet"
}

$runResult = Invoke-Probe -ExePath $probePath -Arguments $probeArgs
if (-not $Quiet) {
    $runResult.Lines | ForEach-Object { Write-Host $_ }
}

if ($runResult.ExitCode -ne 0) {
    throw "探针运行失败，退出码 $($runResult.ExitCode)。"
}

$matchedEvents = Get-IntValue -Table $runResult.Values -Key "MATCHED_EVENT_COUNT"
$sendOk = Get-IntValue -Table $runResult.Values -Key "SEND_SEQUENCE_OK"

if (-not $ManualInput -and $sendOk -ne 1) {
    throw "虚拟鼠标发送序列失败，无法连接驱动或发送报告。"
}

if ($matchedEvents -le 0) {
    throw "未观察到来自虚拟鼠标驱动的 Raw Input 事件。"
}

Write-Host "验证通过：驱动已安装、Raw Input 枚举可见、并成功收到虚拟鼠标事件。"
