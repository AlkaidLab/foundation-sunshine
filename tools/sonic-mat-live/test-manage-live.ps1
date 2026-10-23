param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh operator test output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$control=Join-Path $PSScriptRoot manage-live.ps1
. "$PSScriptRoot/deployment.ps1"
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
@{result='PASS';scope='Operator state/identity/hash/duration; trusted/read-only deployment ACL policy and reparse rejection';nativeCapture=$false;nativeReadyFixtureOnly=$true;sourceHashes=@('deployment.ps1','manage-live.ps1','test-manage-live.ps1')|ForEach-Object {@{name=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $_)).Hash.ToLower()}}}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$out/proof.json"
Write-Host 'PASS operator lifecycle and deployment ACL/reparse guards; nativeCapture=0'
