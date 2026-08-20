[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScriptsDirectory,

    [string]$EvidencePath = (Join-Path $PWD "vdd-rc-evidence.json"),

    [switch]$AllowDestructive,
    [switch]$KeepInstalled,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-VddBatch {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [string[]]$Arguments = @()
    )

    $lines = @(& $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output   = $lines
    }
}

function ConvertFrom-VddProbe {
    param([Parameter(Mandatory = $true)]$Result)

    $values = @{}
    foreach ($line in $Result.Output) {
        if ($line -match '^([A-Z_]+)=(.*)$') {
            $values[$matches[1]] = $matches[2].Trim()
        }
    }

    if ($Result.ExitCode -ne 0 -or $values['VDD_PROBE_OK'] -ne '1') {
        throw "VDD probe failed with exit code $($Result.ExitCode)."
    }

    return [ordered]@{
        device_present  = $values['VDD_DEVICE_PRESENT'] -eq '1'
        current_version = [string]$values['CURRENT_VDD_VERSION']
        current_status  = [string]$values['CURRENT_VDD_STATUS']
        current_problem = [string]$values['CURRENT_VDD_PROBLEM']
        bundled_version = [string]$values['BUNDLED_VDD_VERSION']
        raw_output      = $Result.Output
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ZakoDriverPackages {
    return @(
        Get-WindowsDriver -Online -All -ErrorAction Stop |
            Where-Object { (Split-Path -Leaf $_.OriginalFileName) -ieq 'ZakoVDD.inf' } |
            Select-Object Driver, OriginalFileName, ProviderName, Version, Date
    )
}

function Write-Evidence {
    param([Parameter(Mandatory = $true)]$Evidence)

    $parent = Split-Path -Parent $EvidencePath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $Evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
}

$scriptsPath = (Resolve-Path -LiteralPath $ScriptsDirectory).Path
$installScript = Join-Path $scriptsPath 'install-vdd.bat'
$uninstallScript = Join-Path $scriptsPath 'uninstall-vdd.bat'
if (-not (Test-Path -LiteralPath $installScript)) {
    throw "install-vdd.bat was not found under $scriptsPath"
}
if (-not (Test-Path -LiteralPath $uninstallScript)) {
    throw "uninstall-vdd.bat was not found under $scriptsPath"
}

$evidence = [ordered]@{
    schema_version    = 1
    started_at_utc   = [DateTime]::UtcNow.ToString('o')
    computer_name    = $env:COMPUTERNAME
    scripts_directory = $scriptsPath
    validate_only    = [bool]$ValidateOnly
    keep_installed   = [bool]$KeepInstalled
    result           = 'running'
    failure          = ''
    before           = $null
    packages_before  = $null
    install          = $null
    after_install    = $null
    packages_after_install = $null
    uninstall        = $null
    after_uninstall  = $null
    packages_after_uninstall = $null
}

$exitCode = 0
try {
    $evidence.before = ConvertFrom-VddProbe (Invoke-VddBatch -Path $installScript -Arguments @('--probe-only'))

    if ($ValidateOnly) {
        $evidence.result = 'validation_only'
    }
    else {
        if (-not $AllowDestructive) {
            throw 'Destructive driver verification requires -AllowDestructive on a disposable Windows host.'
        }
        if (-not (Test-IsAdministrator)) {
            throw 'Run this script from an elevated PowerShell session.'
        }
        $evidence.packages_before = @(Get-ZakoDriverPackages)
        if ($evidence.before.device_present) {
            throw 'The host is not clean: Root\ZakoVDD is already present.'
        }
        if ($evidence.packages_before.Count -ne 0) {
            throw 'The host is not clean: a ZakoVDD package already exists in DriverStore.'
        }

        $installResult = Invoke-VddBatch -Path $installScript
        $evidence.install = [ordered]@{
            exit_code  = $installResult.ExitCode
            raw_output = $installResult.Output
        }
        if ($installResult.ExitCode -ne 0) {
            throw "VDD installation failed with exit code $($installResult.ExitCode)."
        }

        $evidence.after_install = ConvertFrom-VddProbe (Invoke-VddBatch -Path $installScript -Arguments @('--probe-only'))
        $evidence.packages_after_install = @(Get-ZakoDriverPackages)
        if (-not $evidence.after_install.device_present -or
            $evidence.after_install.current_status -ne 'OK' -or
            $evidence.after_install.current_problem -ne '0') {
            throw 'VDD did not reach a healthy PnP state after installation.'
        }
        if ($evidence.packages_after_install.Count -eq 0) {
            throw 'ZakoVDD did not appear in DriverStore after installation.'
        }

        if (-not $KeepInstalled) {
            $uninstallResult = Invoke-VddBatch -Path $uninstallScript
            $evidence.uninstall = [ordered]@{
                exit_code  = $uninstallResult.ExitCode
                raw_output = $uninstallResult.Output
            }
            if ($uninstallResult.ExitCode -ne 0) {
                throw "VDD uninstall failed with exit code $($uninstallResult.ExitCode)."
            }

            $evidence.after_uninstall = ConvertFrom-VddProbe (Invoke-VddBatch -Path $installScript -Arguments @('--probe-only'))
            $evidence.packages_after_uninstall = @(Get-ZakoDriverPackages)
            if ($evidence.after_uninstall.device_present) {
                throw 'Root\ZakoVDD is still present after uninstall.'
            }
            if ($evidence.packages_after_uninstall.Count -ne 0) {
                throw 'A ZakoVDD package is still present in DriverStore after uninstall.'
            }
        }

        $evidence.result = 'passed'
    }
}
catch {
    $exitCode = 1
    $evidence.result = 'failed'
    $evidence.failure = $_.Exception.Message
}
finally {
    $evidence.completed_at_utc = [DateTime]::UtcNow.ToString('o')
    Write-Evidence -Evidence $evidence
    Write-Host "VDD_RC_RESULT=$($evidence.result)"
    Write-Host "VDD_RC_EVIDENCE=$EvidencePath"
    if ($evidence.failure) {
        Write-Error $evidence.failure -ErrorAction Continue
    }
}

exit $exitCode
