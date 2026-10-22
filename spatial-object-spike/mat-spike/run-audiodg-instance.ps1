param(
  [string]$Build="$PSScriptRoot/instance-build02",
  [string]$Output="$PSScriptRoot/instance-run01",
  [switch]$ActivationOnly
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$exe=(Resolve-Path -LiteralPath (Join-Path $Build 'audiodg-instance-probe.exe')).Path
$helper=(Resolve-Path -LiteralPath (Join-Path $Build 'mat_instance_stage_a_v1.dll')).Path
if((Get-FileHash -LiteralPath 'C:\Windows\System32\audioeng.dll').Hash -ne '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3'){
  throw 'Unverified audioeng version; do not inject'
}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$prefLog=& $exe (Join-Path $out 'preflight')
$prefExit=$LASTEXITCODE
$prefLog | Set-Content -LiteralPath (Join-Path $out 'preflight.stdout.txt') -Encoding utf8
$prefLog | Write-Output
if($prefExit -ne 0){Write-Output 'BLOCKED: no remote code started. Reopen Codex elevated; no UAC/security change was attempted.';exit $prefExit}
if(-not $ActivationOnly){Write-Output 'Preflight complete; no injection requested. Stage A requires -ActivationOnly.';exit 0}
$package=Get-AppxPackage -Name DolbyLaboratories.DolbyAccess
if(@($package).Count -ne 1){throw 'Expected one installed Dolby Access package'}
$vendor=Join-Path $package.InstallLocation 'DLLs\x64'
$expected=@{
 'DolbyMatEnc.dll'='963697FB61EE302699B294B1B6CB59AB42E5EAE3A555F6674ED54FC3A3CFACEA'
 'DolbyMatEngine.dll'='342CBC58A19AFA2B5B58CEF408EF91DCC06C68829520286ACABB7178EDA7F569'
}
foreach($name in $expected.Keys){
  if((Get-FileHash -LiteralPath (Join-Path $vendor $name)).Hash -ne $expected[$name]){throw "Unverified vendor file: $name"}
}
$deploymentRoot='C:\ProgramData\SpatialObjectSpike'
if(-not(Test-Path -LiteralPath $deploymentRoot)){New-Item -ItemType Directory -Path $deploymentRoot | Out-Null}
$deployed=Join-Path $deploymentRoot 'mat_instance_stage_a_v1.dll'
$helperHash=(Get-FileHash -LiteralPath $helper).Hash
if(-not(Test-Path -LiteralPath $deployed)){Copy-Item -LiteralPath $helper -Destination $deployed}
if((Get-FileHash -LiteralPath $deployed).Hash -ne $helperHash){throw 'Do not overwrite a deployed helper that may already be loaded'}
[ordered]@{
 stage='A activation only';helper=$deployed;helper_sha256=$helperHash;vendor=$vendor
 endpoint_changes=$false;hooks=$false;encoder_initialization=$false;object_submission=$false
 warning='Helper and vendor module references remain mapped until audiodg naturally exits. A timeout cannot safely interrupt a vendor call; audiodg will not be terminated.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'launch.json') -Encoding utf8
$stdout=Join-Path $out 'probe.stdout.txt';$stderr=Join-Path $out 'probe.stderr.txt'
$argLine='"'+(Join-Path $out 'activation')+'" "'+$deployed+'" "'+$vendor+'"'
$child=Start-Process -FilePath $exe -ArgumentList $argLine -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$child.WaitForExit();$child.Refresh()
Get-Content -LiteralPath $stdout
$summary=Join-Path $out 'activation\summary.json'
if(Test-Path -LiteralPath $summary){Get-Content -LiteralPath $summary}
if($child.ExitCode -ne 0){throw "Stage A collector exit=$($child.ExitCode); inspect saved records before retrying"}
