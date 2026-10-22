$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'audiodg-access-elevated.log'
& (Join-Path $PSScriptRoot 'spatial_probe.exe') --access-only | Set-Content -LiteralPath $out -Encoding UTF8
Get-Process audiodg -ErrorAction SilentlyContinue | ForEach-Object {
    "PID=$($_.Id) loaded modules:" | Add-Content -LiteralPath $out -Encoding UTF8
    $_.Modules | ForEach-Object { $_.ModuleName } | Add-Content -LiteralPath $out -Encoding UTF8
}
