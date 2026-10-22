param(
  [string]$Build="$PSScriptRoot/direct-build04",
  [string]$Output="$PSScriptRoot/direct-run01"
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$exe=(Resolve-Path -LiteralPath (Join-Path $Build 'direct-encoder-probe.exe')).Path
$out=(New-Item -ItemType Directory -Path $Output).FullName
$package=Get-AppxPackage -Name DolbyLaboratories.DolbyAccess
if(@($package).Count -ne 1){throw 'Expected one currently installed Dolby Access package'}
$dllDir=Join-Path $package.InstallLocation 'DLLs\x64'
$inventory=@()
foreach($name in @('DolbyMatEnc.dll','DolbyMatEngine.dll')) {
  $path=Join-Path $dllDir $name
  $file=Get-Item -LiteralPath $path
  $acl=Get-Acl -LiteralPath $path
  $signature=Get-AuthenticodeSignature -LiteralPath $path
  $inventory+= [ordered]@{
    path=$path;bytes=$file.Length;version=$file.VersionInfo.FileVersion
    sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    sddl=$acl.Sddl;signature_status=[string]$signature.Status
    signer=$signature.SignerCertificate.Subject
  }
}
$inventory | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'installed-components.json') -Encoding utf8
$started=Get-Date
$stdout=Join-Path $out 'probe.stdout.txt';$stderr=Join-Path $out 'probe.stderr.txt'
$process=Start-Process -FilePath $exe -ArgumentList ('"'+$dllDir+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$timedOut=-not $process.WaitForExit(30000)
if($timedOut){$process.Kill();$process.WaitForExit()}
$process.Refresh()
$log=Get-Content -LiteralPath $stdout -Raw
$complete=$log.Contains('DONE activation_only=1 encoded_bytes=0 endpoint_calls=0')
$loadDenied=([regex]::Matches($log,'LoadLibraryExW hr=0x80070005')).Count
$accessDenied=([regex]::Matches($log,'CreateFile READ_EXECUTE hr=0x80070005')).Count
$unregistered=([regex]::Matches($log,'(?:CoCreateInstance|RoGetActivationFactory) hr=0x80040154')).Count
$mftCount=[regex]::Match($log,'encoder_count=(\d+)')
$matFilters=@([regex]::Matches($log,'MAT_FILTER (\{[^}]+\}) count=(\d+)') | ForEach-Object {
  [ordered]@{subtype=$_.Groups[1].Value;count=[int]$_.Groups[2].Value}
})
$result=[ordered]@{
  started=$started.ToString('o');finished=(Get-Date).ToString('o')
  process_exit=$process.ExitCode;timed_out=$timedOut;diagnostic_completed=$complete
  package_name=$package.PackageFullName;probe_sha256=(Get-FileHash -LiteralPath $exe).Hash
  enumerated_audio_encoder_mfts=$(if($mftCount.Success){[int]$mftCount.Groups[1].Value}else{$null})
  mat_output_filters=$matFilters
  public_ac3_activation_control_ok=$log.Contains('Control CoCreateInstance AC3 IMFTransform hr=0x00000000')
  system_dll_load_control_ok=$log.Contains('Control LoadLibraryExW propsys.dll hr=0x00000000')
  unregistered_activation_calls=$unregistered;denied_dll_loads=$loadDenied;denied_read_execute_opens=$accessDenied
  encoder_initialization_attempted=$false;object_submission_attempted=$false
  mat_encoding_verified=$false;endpoint_independence_verified=$false
  endpoint_calls=0;encoded_bytes=0
  security_settings_modified=$false;vendor_binaries_copied=$false;drivers_installed=$false
  note='Activation diagnostic only. Failure is not a proof that all private invocation methods are impossible. No license/access checks were bypassed.'
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
Get-Content -LiteralPath $stdout
$result | ConvertTo-Json -Depth 5
if($timedOut -or $process.ExitCode -ne 0 -or -not $complete){throw 'Diagnostic did not complete normally'}
