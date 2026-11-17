#requires -version 5.1
param([Parameter(Mandatory)][string]$Payload)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -Raw -LiteralPath (Join-Path $Payload 'payload.json') | ConvertFrom-Json
$tools = @(Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName -Descending)
if ($tools.Count -eq 0) { throw 'Windows SDK signtool is required; package signatures cannot be skipped' }
$signtool = $tools[0].FullName
$results = @()
foreach ($file in $manifest.files.PSObject.Properties) {
  $path = Join-Path $Payload $file.Name
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $file.Value.sha256) { throw "Hash mismatch: $($file.Name)" }
  if ($file.Name -like 'driver/*') {
    $cat = [IO.Path]::ChangeExtension($path, '.cat')
    # usbip-slim-signtool-20261117, Verifying Examples: verify both the
    # catalog's kernel policy and each INF/SYS member's catalog binding.
    if ($path.EndsWith('.cat')) { & $signtool verify /kp $path }
    else { & $signtool verify /kp /c $cat $path }
    if ($LASTEXITCODE -ne 0) { throw "Kernel catalog verification failed: $($file.Name)" }
    $results += [pscustomobject]@{ file = $file.Name; sha256 = $file.Value.sha256; verification = 'signtool /kp: passed' }
  } elseif ($file.Value.source -like 'runtime/*') {
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike '*O=Microsoft Corporation*') {
      throw "Microsoft runtime signature verification failed: $($file.Name)"
    }
    $results += [pscustomobject]@{ file = $file.Name; sha256 = $file.Value.sha256; verification = 'Microsoft Authenticode: valid' }
  } else {
    $results += [pscustomobject]@{ file = $file.Name; sha256 = $file.Value.sha256; verification = 'pinned upstream SHA-256: passed' }
  }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Payload '..\usbip-signature-results.json') -Encoding UTF8
Write-Host "USB/IP payload: $($results.Count) file hashes and all catalog members verified"
