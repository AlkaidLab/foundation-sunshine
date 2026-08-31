param(
  [Parameter(Mandatory = $true)]
  [string] $ExecutablePath,

  [switch] $IncludeDescendants
)

$targetPath = [System.IO.Path]::GetFullPath($ExecutablePath)
$matchingProcesses = @(
  Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
      return $false
    }

    try {
      $processPath = [System.IO.Path]::GetFullPath($_.ExecutablePath)
      return [string]::Equals($processPath, $targetPath, [System.StringComparison]::OrdinalIgnoreCase)
    }
    catch {
      return $false
    }
  }
)

$taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
foreach ($process in $matchingProcesses) {
  $arguments = @('/f', '/pid', [string] $process.ProcessId)
  if ($IncludeDescendants) {
    $arguments = @('/t') + $arguments
  }
  & $taskkill @arguments | Out-Null
}
