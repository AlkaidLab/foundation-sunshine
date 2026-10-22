param(
 [string]$Build="$PSScriptRoot/encode-build03",
 [ValidatePattern('^[a-zA-Z0-9_-]+[.]dll$')][string]$HelperName='mat_encode_stage_b_v3.dll',
 [string]$InputDirectory="$PSScriptRoot/encode-input01",
 [string]$Output="$PSScriptRoot/encode-run03"
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
try{
 $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
 if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'This one-shot probe requires the approved UAC elevation'}
 $exe=(Resolve-Path -LiteralPath (Join-Path $Build 'mat-encode-probe.exe')).Path
 $helper=(Resolve-Path -LiteralPath (Join-Path $Build $HelperName)).Path
 $inputFile=(Resolve-Path -LiteralPath (Join-Path $InputDirectory 'input.bin')).Path
 $inputInfo=Get-Content -LiteralPath (Join-Path $InputDirectory 'summary.json') -Raw | ConvertFrom-Json
 if((Get-FileHash -LiteralPath $inputFile).Hash -ne $inputInfo.input_sha256){throw 'Input hash mismatch'}
 if((Get-FileHash -LiteralPath 'C:\Windows\System32\audioeng.dll').Hash -ne '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3'){throw 'Unverified audioeng version'}
 $prefLog=& $exe (Join-Path $out 'preflight');$prefExit=$LASTEXITCODE
 $prefLog | Set-Content -LiteralPath (Join-Path $out 'preflight.stdout.txt') -Encoding utf8
 if($prefExit){throw "Access preflight failed: $prefExit"}
 $package=Get-AppxPackage -Name DolbyLaboratories.DolbyAccess
 if(@($package).Count -ne 1){throw 'Expected one installed Dolby Access package'}
 $vendor=Join-Path $package.InstallLocation 'DLLs\x64'
 $expected=@{
  'DolbyMatEnc.dll'='963697FB61EE302699B294B1B6CB59AB42E5EAE3A555F6674ED54FC3A3CFACEA'
  'DolbyMatEngine.dll'='342CBC58A19AFA2B5B58CEF408EF91DCC06C68829520286ACABB7178EDA7F569'
 }
 foreach($name in $expected.Keys){if((Get-FileHash -LiteralPath (Join-Path $vendor $name)).Hash -ne $expected[$name]){throw "Unverified vendor component $name"}}
 $deployed=Join-Path 'C:\ProgramData\SpatialObjectSpike' $HelperName
 $helperHash=(Get-FileHash -LiteralPath $helper).Hash
 if(-not(Test-Path -LiteralPath $deployed)){Copy-Item -LiteralPath $helper -Destination $deployed}
 if((Get-FileHash -LiteralPath $deployed).Hash -ne $helperHash){throw 'Pinned helper hash mismatch; do not overwrite'}
 [ordered]@{stage='B bounded initialization/encoding';helper=$deployed;helper_sha256=$helperHash;input=$inputFile;input_sha256=$inputInfo.input_sha256
   elevated=$true;endpoint_changes=$false;hooks=$false;source_seconds_per_case=1;silent_preroll_seconds=0.2;tail_seconds=0.08;cases=2
   warning='Version-specific private ABI experiment. Stop on initialization failure. No forced termination or security bypass. Helper/vendor modules remain mapped until audiodg naturally exits.'
 } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'launch.json') -Encoding utf8
 $stdout=Join-Path $out 'probe.stdout.txt';$stderr=Join-Path $out 'probe.stderr.txt'
 $argLine='"'+(Join-Path $out 'capture')+'" "'+$deployed+'" "'+$vendor+'" "'+$inputFile+'"'
 $child=Start-Process -FilePath $exe -ArgumentList $argLine -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
 $held=$child.Handle;$child.WaitForExit();$child.Refresh()
 [ordered]@{probe_exit=$child.ExitCode;finished=(Get-Date).ToString('o');mat_verified=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'exit.json') -Encoding utf8
 if($child.ExitCode -ne 0){throw "Probe exited $($child.ExitCode); inspect records before retrying"}
}catch{
 $_.Exception.Message | Set-Content -LiteralPath (Join-Path $out 'launcher-error.txt') -Encoding utf8
 throw
}
