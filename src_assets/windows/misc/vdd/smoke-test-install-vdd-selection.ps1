param(
    [string]$InstallScript = (Join-Path $PSScriptRoot 'install-vdd.bat'),
    [string]$HelperScript = (Join-Path $PSScriptRoot 'vdd-device-helper.ps1')
)

$ErrorActionPreference = 'Stop'

function New-TestLayout {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [bool]$IncludeLatest = $true,
        [bool]$IncludeWin10 = $true,
        [bool]$IncludeRootPayload = $false
    )

    $driverRoot = Join-Path $Root 'driver'
    New-Item -ItemType Directory -Force -Path $driverRoot | Out-Null
    Set-Content -Path (Join-Path $driverRoot 'vdd_settings.xml') -Value '<settings />' -Encoding Ascii

    if ($IncludeLatest) {
        $latest = Join-Path $driverRoot 'latest'
        New-Item -ItemType Directory -Force -Path $latest | Out-Null
        Set-Content -Path (Join-Path $latest 'ZakoVDD.inf') -Value 'latest' -Encoding Ascii
    }

    if ($IncludeWin10) {
        $win10 = Join-Path $driverRoot 'win10'
        New-Item -ItemType Directory -Force -Path $win10 | Out-Null
        Set-Content -Path (Join-Path $win10 'ZakoVDD.inf') -Value 'win10' -Encoding Ascii
    }

    if ($IncludeRootPayload) {
        Set-Content -Path (Join-Path $driverRoot 'ZakoVDD.inf') -Value 'root' -Encoding Ascii
    }
}

function Invoke-ResolveOnly {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptCopy,

        [string]$BuildOverride
    )

    $wrapper = Join-Path ([System.IO.Path]::GetDirectoryName($ScriptCopy)) 'invoke-resolve-only.cmd'
    $wrapperLines = @(
        '@echo off',
        'setlocal'
    )

    if ($null -ne $BuildOverride) {
        $wrapperLines += ('set "VDD_TEST_WIN_BUILD={0}"' -f $BuildOverride)
    }

    $wrapperLines += ('call "{0}" --resolve-only' -f $ScriptCopy)
    $wrapperLines += 'exit /b %ERRORLEVEL%'

    Set-Content -Path $wrapper -Value $wrapperLines -Encoding Ascii

    $stdout = Join-Path ([System.IO.Path]::GetDirectoryName($ScriptCopy)) 'invoke-resolve-only.stdout.txt'
    $stderr = Join-Path ([System.IO.Path]::GetDirectoryName($ScriptCopy)) 'invoke-resolve-only.stderr.txt'

    $process = Start-Process -FilePath $wrapper `
        -Wait `
        -NoNewWindow `
        -PassThru `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr

    $stdoutText = if (Test-Path $stdout) { Get-Content -Path $stdout -Raw } else { '' }
    $stderrText = if (Test-Path $stderr) { Get-Content -Path $stderr -Raw } else { '' }
    $output = ($stdoutText + $stderrText).Trim()
    $exitCode = $process.ExitCode

    if ($exitCode -ne 0) {
        throw "Resolve-only invocation failed with exit code $exitCode`n$output"
    }

    return $output
}

function Get-ResolvedDriverDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Output
    )

    $line = ($Output -split "`r?`n" | Where-Object { $_ -like 'RESOLVED_DRIVER_DIR=*' } | Select-Object -First 1)
    if (-not $line) {
        throw "RESOLVED_DRIVER_DIR was not found in output:`n$Output"
    }

    return $line.Substring('RESOLVED_DRIVER_DIR='.Length).Trim()
}

if (-not (Test-Path $InstallScript)) {
    throw "install-vdd.bat not found: $InstallScript"
}
if (-not (Test-Path $HelperScript)) {
    throw "vdd-device-helper.ps1 not found: $HelperScript"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sunshine-vdd-smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

try {
    $cases = @(
        @{
            Name = 'Win10 build selects win10 payload'
            Build = '19045'
            ExpectRelative = 'driver\win10'
            IncludeLatest = $true
            IncludeWin10 = $true
            IncludeRootPayload = $false
        },
        @{
            Name = 'Win11 build selects latest payload'
            Build = '22000'
            ExpectRelative = 'driver\latest'
            IncludeLatest = $true
            IncludeWin10 = $true
            IncludeRootPayload = $false
        },
        @{
            Name = 'Future build still selects latest'
            Build = '26200'
            ExpectRelative = 'driver\latest'
            IncludeLatest = $true
            IncludeWin10 = $true
            IncludeRootPayload = $false
        },
        @{
            Name = 'Invalid build falls back to win10'
            Build = 'banana'
            ExpectRelative = 'driver\win10'
            IncludeLatest = $true
            IncludeWin10 = $true
            IncludeRootPayload = $false
        },
        @{
            Name = 'Legacy single-root payload still works'
            Build = 'banana'
            ExpectRelative = 'driver'
            IncludeLatest = $false
            IncludeWin10 = $false
            IncludeRootPayload = $true
        }
    )

    $results = foreach ($case in $cases) {
        $safeName = ($case.Name -replace '[^A-Za-z0-9]+', '-').Trim('-')
        $sandbox = Join-Path $tempRoot $safeName
        New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

        $scriptCopy = Join-Path $sandbox 'install-vdd.bat'
        Copy-Item -Path $InstallScript -Destination $scriptCopy -Force
        Copy-Item -Path $HelperScript -Destination (Join-Path $sandbox 'vdd-device-helper.ps1') -Force

        New-TestLayout -Root $sandbox `
            -IncludeLatest $case.IncludeLatest `
            -IncludeWin10 $case.IncludeWin10 `
            -IncludeRootPayload $case.IncludeRootPayload

        $output = Invoke-ResolveOnly -ScriptCopy $scriptCopy -BuildOverride $case.Build
        $resolvedDir = Get-ResolvedDriverDir -Output $output
        $expectedDir = Join-Path $sandbox $case.ExpectRelative

        $resolvedFull = [System.IO.Path]::GetFullPath($resolvedDir).TrimEnd('\')
        $expectedFull = [System.IO.Path]::GetFullPath($expectedDir).TrimEnd('\')

        if ($resolvedFull -ne $expectedFull) {
            throw "Case '$($case.Name)' failed.`nExpected: $expectedFull`nActual:   $resolvedFull`nOutput:`n$output"
        }

        [pscustomobject]@{
            Case = $case.Name
            Build = $case.Build
            Selected = $resolvedFull
            Status = 'PASS'
        }
    }

    $results | Format-Table -AutoSize
    Write-Host "All VDD payload selection smoke tests passed."
}
finally {
    if (Test-Path $tempRoot) {
        Remove-Item -Path $tempRoot -Recurse -Force
    }
}
