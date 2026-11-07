param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh operator test output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$control=Join-Path $PSScriptRoot manage-live.ps1
. "$PSScriptRoot/deployment.ps1"
# Launch-time validation needs only the three runtime artifacts. Source, docs
# and test products may have moved or changed since the build was recorded.
$package=Join-Path $out 'runtime-only';New-Item -ItemType Directory -Path $package|Out-Null
$runtimeNames=@('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll')
$runtime=@($runtimeNames|ForEach-Object {
 $path=Join-Path $package $_;Set-Content -LiteralPath $path -Value 'hash fixture, never executed'
 @{name=$_;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLower()}
})
Assert-MatBuildArtifacts $package @{binaries=$runtime}
Assert-MatBuildArtifacts $package @{binaries=$runtime+@(@{name='absent-test.exe';sha256='0'*64});sources=@(@{path='missing-source-or-doc';sha256='0'*64})}
foreach($case in @('MissingRecord','DuplicateRecord','InvalidHash','WrongHash','MissingFile','CorruptFile')){
 $entries=@($runtime|ForEach-Object {@{name=$_.name;sha256=$_.sha256}})
 switch($case){
  MissingRecord {$entries=@($entries|Select-Object -Skip 1)}
  DuplicateRecord {$entries+=@($entries[0])}
  InvalidHash {$entries[0].sha256='not-a-hash'}
  WrongHash {$entries[0].sha256='0'*64}
  MissingFile {Remove-Item -LiteralPath (Join-Path $package $runtimeNames[0])}
  CorruptFile {Set-Content -LiteralPath (Join-Path $package $runtimeNames[0]) -Value 'corrupted fixture'}
 }
 $rejected=$false;try{Assert-MatBuildArtifacts $package @{binaries=$entries}}catch{$rejected=$true}
 if(!$rejected){throw "Invalid runtime package accepted: $case"}
 if($case -in @('MissingFile','CorruptFile')){Set-Content -LiteralPath (Join-Path $package $runtimeNames[0]) -Value 'hash fixture, never executed'}
}
# A build file swapped after the manifest check must not be deployed, and an
# existing deployment must match the manifest rather than its own content.
$deployTarget=Join-Path $out 'deploy-copy.dll';$source=Join-Path $package $runtimeNames[1]
Copy-MatDeploymentFile $source $deployTarget $runtime[1].sha256
Copy-MatDeploymentFile $source $deployTarget $runtime[1].sha256
Set-Content -LiteralPath $source -Value 'swapped after manifest check'
$swapped=Join-Path $out 'deploy-swapped.dll'
$rejected=$false;try{Copy-MatDeploymentFile $source $swapped $runtime[1].sha256}catch{if($_.Exception.Message -notmatch 'manifest'){throw};$rejected=$true}
if(!$rejected -or (Test-Path -LiteralPath $swapped)){throw 'Swapped build binary was deployed'}
Set-Content -LiteralPath $deployTarget -Value 'tampered deployment'
$rejected=$false;try{Copy-MatDeploymentFile $source $deployTarget $runtime[1].sha256}catch{$rejected=$true}
if(!$rejected -or !(Test-Path -LiteralPath $deployTarget)){throw 'Tampered deployment accepted or removed'}
Set-Content -LiteralPath $source -Value 'hash fixture, never executed'
# Policy tests use in-memory ACLs and own workspace paths; no Program Files writes.
$trustedAcl=[Security.AccessControl.DirectorySecurity]::new()
$trustedAcl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
foreach($sid in @('S-1-5-32-544','S-1-5-18')){
 $trustedAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),'FullControl','Allow'))
}
$trustedAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'ReadAndExecute','Allow'))
Assert-MatDeploymentAcl $trustedAcl
foreach($case in @('Owner','Write','Delete','ChangePermissions','TakeOwnership')){
 $badAcl=[Security.AccessControl.DirectorySecurity]::new()
 $badAcl.SetSecurityDescriptorBinaryForm($trustedAcl.GetSecurityDescriptorBinaryForm())
 $user=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-545')
 if($case -eq 'Owner'){$badAcl.SetOwner($user)}else{$badAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($user,[Security.AccessControl.FileSystemRights]$case,'Allow'))}
 $rejected=$false;try{Assert-MatDeploymentAcl $badAcl}catch{$rejected=$true}
 if(!$rejected){throw "Unsafe deployment ACL accepted: $case"}
}
Assert-MatDeploymentEntry ([Environment]::GetFolderPath('ProgramFiles'))
$junctionTarget=Join-Path $out 'junction-target';New-Item -ItemType Directory -Path $junctionTarget|Out-Null
$junction=Join-Path $out 'deployment-link';New-Item -ItemType Junction -Path $junction -Target $junctionTarget|Out-Null
$rejected=$false;try{Assert-MatDeploymentEntry $junction}catch{if($_.Exception.Message -notmatch 'reparse'){throw};$rejected=$true}
if(!$rejected){throw 'Deployment reparse point accepted'}
$current=Get-Process -Id $PID
$image=$current.Path
# Only own-process metadata is used. No audiodg, native attach or source generator.
$state=@{schema='native-plugin-live-v1';publisherPid=2147483000;publisherCreated='1';producer=$image;producerSha256=(Get-FileHash $image).Hash.ToLower();
 enginePid=$PID;engineCreated=$current.StartTime.ToFileTimeUtc().ToString();pipe='\\.\pipe\SunshineMatLive-operator-test';stopEvent='Local\SunshineMatLiveStop-operator-test';captureLog="$out/capture.log"}
function Save-State {$state|ConvertTo-Json|Set-Content -LiteralPath "$out/capture.json"}
function Check-Status([string]$Expected){Save-State;$actual=(& $control -Action Status -Metadata "$out/capture.json")|ConvertFrom-Json;if($actual.state -ne $Expected){throw "Status expected $Expected, received $($actual.state)"}}
Set-Content -LiteralPath $state.captureLog -Value ''
Check-Status 'ExitedUnverified'
Set-Content -LiteralPath $state.captureLog -Value 'HOOK_STOP restored=1 nativeStatus=2 nativeError=0 entries=6'
Check-Status 'StoppedVerified'
(& $control -Action Stop -Metadata "$out/capture.json")|Out-Null
Set-Content -LiteralPath $state.captureLog -Value @('STOP cycles=0 failed=1 error=source-stall restored=1','HOOK_STOP restored=1 nativeStatus=2 nativeError=0 entries=6')
Check-Status 'Faulted'
Set-Content -LiteralPath $state.captureLog -Value @('COMPONENT name=audioeng.dll support=unsupported version=10.0.1.2 sha256=00','COMPONENT name=DolbyAccess support=missing version=unknown sha256=none','FAILED Unsupported system component; see COMPONENT lines win32=0')
Check-Status 'Unsupported'
$state.publisherPid=$PID;$state.publisherCreated=$current.StartTime.ToFileTimeUtc().ToString()
Set-Content -LiteralPath $state.captureLog -Value ''
Check-Status 'WaitingForObjects'
Set-Content -LiteralPath $state.captureLog -Value 'NATIVE_MAT_READY source=test-state-only'
Check-Status 'NativeReady'
$state.publisherCreated='1';Save-State
$rejected=$false;try{& $control -Action Stop -Metadata "$out/capture.json"|Out-Null}catch{$rejected=$true}
if(!$rejected){throw 'Changed process identity was accepted'}
$state.publisherPid=2147483000;$state.producerSha256='0'*64;Save-State
$rejected=$false;try{& $control -Action Status -Metadata "$out/capture.json"|Out-Null}catch{$rejected=$true}
if(!$rejected){throw 'Changed publisher hash was accepted'}
foreach($options in @(@{},@{Continuous=$true;Seconds=8})){
 $rejected=$false;try{& $control -Action Start -Build 'nonexistent' -Output "$out/not-created" -EnginePid 1 -EngineCreated 1 @options|Out-Null}catch{if($_.Exception.Message -notmatch 'exactly one'){throw};$rejected=$true}
 if(!$rejected -or (Test-Path "$out/not-created")){throw 'Invalid capture mode reached deployment/start'}
}
@{result='PASS';scope='Runtime package, operator lifecycle and deployment guards';nativeCapture=$false}|ConvertTo-Json|Set-Content -LiteralPath "$out/proof.json"
Write-Host 'PASS operator lifecycle, manifest-bound deployment and ACL/reparse guards; nativeCapture=0'
