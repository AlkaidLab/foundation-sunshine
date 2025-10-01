# Generate SHA256 checksums for release packages
# Usage: .\generate-checksums.ps1 [-Path <directory>] [-Output <file>]

param(
    [string]$Path = ".",
    [string]$Output = "SHA256SUMS.txt"
)

Write-Host "Generating SHA256 checksums..." -ForegroundColor Cyan
Write-Host "Directory: $Path" -ForegroundColor Gray
Write-Host ""

# Get all package files
$packages = Get-ChildItem -Path $Path -Include @(
    "*.exe",
    "*.msi", 
    "*.zip",
    "*.7z"
) -Recurse

if ($packages.Count -eq 0) {
    Write-Host "No package files found!" -ForegroundColor Red
    exit 1
}

# Calculate checksums
$checksums = @()
foreach ($package in $packages) {
    Write-Host "Computing: $($package.Name)..." -ForegroundColor Yellow
    
    $hash = Get-FileHash -Path $package.FullName -Algorithm SHA256
    $relativePath = Resolve-Path -Path $package.FullName -Relative
    
    $checksums += [PSCustomObject]@{
        Hash = $hash.Hash
        File = $package.Name
        Size = "{0:N2} MB" -f ($package.Length / 1MB)
    }
}

# Create output content
$content = @"
# Sunshine Release Package Checksums
# Generated: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss UTC")
# Algorithm: SHA256

Verify your downloads with:
  Windows (PowerShell): Get-FileHash -Algorithm SHA256 <filename>
  Linux/macOS:          shasum -a 256 <filename>

================================================================================

"@

foreach ($item in $checksums) {
    $content += "$($item.Hash.ToLower())  $($item.File)`n"
    Write-Host "  $($item.Hash.ToLower())" -ForegroundColor Green
    Write-Host "  $($item.File) ($($item.Size))" -ForegroundColor White
    Write-Host ""
}

# Write to file
$outputPath = Join-Path -Path $Path -ChildPath $Output
$content | Out-File -FilePath $outputPath -Encoding UTF8 -NoNewline

Write-Host "Checksums saved to: $outputPath" -ForegroundColor Cyan
Write-Host ""
Write-Host "Summary:" -ForegroundColor Cyan
Write-Host "  Files processed: $($checksums.Count)" -ForegroundColor White
Write-Host "  Total size: $("{0:N2} MB" -f (($packages | Measure-Object -Property Length -Sum).Sum / 1MB))" -ForegroundColor White

# Also create a JSON version for automation
$jsonOutput = Join-Path -Path $Path -ChildPath "checksums.json"
$checksums | ConvertTo-Json -Depth 10 | Out-File -FilePath $jsonOutput -Encoding UTF8

Write-Host "  JSON output: $jsonOutput" -ForegroundColor White

