[CmdletBinding()]
param(
  [string] $HelperScript = (Join-Path $PSScriptRoot 'stop-process-tree.ps1')
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool] $Condition, [string] $Message) {
  if (-not $Condition) {
    throw $Message
  }
}

function Get-ProcessesAtPath([string] $ExecutablePath) {
  $targetPath = [System.IO.Path]::GetFullPath($ExecutablePath)
  return @(
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
      if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
        return $false
      }
      try {
        return [string]::Equals(
          [System.IO.Path]::GetFullPath($_.ExecutablePath),
          $targetPath,
          [System.StringComparison]::OrdinalIgnoreCase
        )
      }
      catch {
        return $false
      }
    }
  )
}

function Wait-ForProcessAtPath([string] $ExecutablePath, [int] $TimeoutSeconds = 10) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    $process = @(Get-ProcessesAtPath $ExecutablePath | Select-Object -First 1)
    if ($process.Count -eq 1) {
      return $process[0]
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "Timed out waiting for process: $ExecutablePath"
}

function Wait-ForNoProcessAtPath([string] $ExecutablePath, [int] $TimeoutSeconds = 10) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    if (@(Get-ProcessesAtPath $ExecutablePath).Count -eq 0) {
      return
    }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "Timed out waiting for process to stop: $ExecutablePath"
}

function Start-TestProcessTree([string] $ParentExecutable, [string] $ChildExecutable, [string] $LauncherScript) {
  $childCommand = 'ping.exe -n 60 127.0.0.1 >nul'
  $launcherLines = @(
    '@echo off',
    ('start "" /b "{0}" /d /s /c "{1}"' -f $ChildExecutable, $childCommand),
    'ping.exe -n 60 127.0.0.1 >nul'
  )
  Set-Content -LiteralPath $LauncherScript -Value $launcherLines -Encoding Ascii
  return Start-Process -FilePath $ParentExecutable `
    -ArgumentList @('/d', '/s', '/c', ('"{0}"' -f $LauncherScript)) `
    -WindowStyle Hidden `
    -PassThru
}

function Stop-TestProcesses([string[]] $ExecutablePaths) {
  $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
  foreach ($executablePath in $ExecutablePaths) {
    foreach ($process in @(Get-ProcessesAtPath $executablePath)) {
      & $taskkill /t /f /pid $process.ProcessId 2>$null | Out-Null
    }
  }
}

if (-not (Test-Path -LiteralPath $HelperScript)) {
  throw "Process cleanup helper not found: $HelperScript"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sunshine-process-smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$parentExecutable = Join-Path $tempRoot 'sunshine-gui-test.exe'
$childExecutable = Join-Path $tempRoot 'setup-test.exe'
$launcherScript = Join-Path $tempRoot 'launch-tree.cmd'
$otherRoot = Join-Path $tempRoot 'other-install'
New-Item -ItemType Directory -Force -Path $otherRoot | Out-Null
$otherExecutable = Join-Path $otherRoot 'sunshine-gui-test.exe'
$otherLauncherScript = Join-Path $otherRoot 'stay-alive.cmd'
Copy-Item -LiteralPath $env:ComSpec -Destination $parentExecutable
Copy-Item -LiteralPath $env:ComSpec -Destination $childExecutable
Copy-Item -LiteralPath $env:ComSpec -Destination $otherExecutable
Set-Content -LiteralPath $otherLauncherScript `
  -Value @('@echo off', 'ping.exe -n 60 127.0.0.1 >nul') `
  -Encoding Ascii

try {
  [void] (Start-Process -FilePath $otherExecutable `
    -ArgumentList @('/d', '/s', '/c', ('"{0}"' -f $otherLauncherScript)) `
    -WindowStyle Hidden `
    -PassThru)
  [void] (Wait-ForProcessAtPath $otherExecutable)

  [void] (Start-TestProcessTree $parentExecutable $childExecutable $launcherScript)
  [void] (Wait-ForProcessAtPath $parentExecutable)
  [void] (Wait-ForProcessAtPath $childExecutable)

  & $HelperScript -ExecutablePath $parentExecutable
  Wait-ForNoProcessAtPath $parentExecutable
  Assert-True -Condition (@(Get-ProcessesAtPath $childExecutable).Count -eq 1) `
    -Message 'Stopping a GUI process must preserve its installer child.'
  Assert-True -Condition (@(Get-ProcessesAtPath $otherExecutable).Count -eq 1) `
    -Message 'Stopping one installation must preserve the same executable name in another directory.'
  Stop-TestProcesses @($childExecutable)
  Wait-ForNoProcessAtPath $childExecutable

  [void] (Start-TestProcessTree $parentExecutable $childExecutable $launcherScript)
  [void] (Wait-ForProcessAtPath $parentExecutable)
  [void] (Wait-ForProcessAtPath $childExecutable)

  & $HelperScript -ExecutablePath $parentExecutable -IncludeDescendants
  Wait-ForNoProcessAtPath $parentExecutable
  Wait-ForNoProcessAtPath $childExecutable
  Assert-True -Condition (@(Get-ProcessesAtPath $otherExecutable).Count -eq 1) `
    -Message 'Recursive cleanup must remain scoped to the requested executable path.'

  Write-Host 'Process cleanup smoke tests passed.'
}
finally {
  Stop-TestProcesses @($childExecutable, $parentExecutable, $otherExecutable)
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
  }
}
