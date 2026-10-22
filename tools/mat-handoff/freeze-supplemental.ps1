param(
 [Parameter(Mandatory=$true)][string]$Verification,
 [Parameter(Mandatory=$true)][string]$BrokerProof,
 [Parameter(Mandatory=$true)][string]$GradleLog,
 [Parameter(Mandatory=$true)][string]$CMakeLog,
 [string]$StrictCompileLog
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$out=(Resolve-Path -LiteralPath $Verification).Path
if(Test-Path -LiteralPath "$out/supplemental.json"){throw 'Supplemental evidence already exists'}
$main=Get-Content -Raw "$out/proof.json" | ConvertFrom-Json
$broker=Get-Content -Raw $BrokerProof | ConvertFrom-Json
$gradle=Get-Content -Raw $GradleLog
$cmake=Get-Content -Raw $CMakeLog
if($main.result -ne 'PASS' -or !$broker.passed -or $broker.native_encoder_called -or $broker.audiodg_accessed){throw 'Missing main/fake-broker acceptance'}
if($gradle -notmatch 'BUILD SUCCESSFUL' -or $cmake -notmatch 'Ran 7 tests' -or $cmake -notmatch '\bOK\b'){throw 'Compilation/configuration acceptance missing'}
$logs=[ordered]@{}
foreach($path in @($BrokerProof,$GradleLog,$CMakeLog)){
 $logs[(Resolve-Path -LiteralPath $path).Path]=(Get-FileHash -LiteralPath $path).Hash.ToLower()
}
if($StrictCompileLog){
 $strict=Get-Content -Raw $StrictCompileLog
 if($strict -notmatch 'PASS strict GCC' -or $strict -notmatch 'PASS strict MSVC'){throw 'Strict compilation evidence missing'}
 $logs[(Resolve-Path -LiteralPath $StrictCompileLog).Path]=(Get-FileHash -LiteralPath $StrictCompileLog).Hash.ToLower()
}
$sourceHashes=[ordered]@{}
foreach($file in @('tests/tools/test_cmake_configuration.py','tools/mat-handoff/freeze-supplemental.ps1',
 'spatial-object-spike/mat-spike/build-spatial-broker.ps1','spatial-object-spike/mat-spike/verify_spatial_broker.py')){
 $sourceHashes[$file]=(Get-FileHash -LiteralPath "$root/$file").Hash.ToLower()
}
[ordered]@{
 result='PASS';scope='Supplemental local no-device checks; source/version context is the main proof, not device acceptance'
 mainProofSha256=(Get-FileHash -LiteralPath "$out/proof.json").Hash.ToLower()
 cmakeConfigurationTests=7;appKotlinJavaCompiled=$true;fullMoonlightApkBuilt=$false;strictCompileChecked=[bool]$StrictCompileLog
 broker=$broker;sourceHashes=$sourceHashes;logHashes=$logs
} | ConvertTo-Json -Depth 12 | Out-File -LiteralPath "$out/supplemental.json" -Encoding utf8
Write-Host "PASS supplemental evidence: $out/supplemental.json"
