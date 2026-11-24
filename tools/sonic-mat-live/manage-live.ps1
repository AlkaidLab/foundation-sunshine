param(
 [ValidateSet('Preflight','Start','Status','Stop')][string]$Action='Preflight',
 [string]$Build,[string]$Output,[string]$Metadata,
 [uint32]$EnginePid,[uint64]$EngineCreated,
 [switch]$Continuous,[ValidateRange(0,60)][int]$Seconds=0,[switch]$Reattach,
 [ValidatePattern('^[0-9a-f]{32}$')][string]$SessionId
)
# Explicit operator action only. No UAC, source generator, endpoint selection,
# installed host edits, automatic restart, forced kill or DLL unload.
$ErrorActionPreference='Stop'
. "$PSScriptRoot/deployment.ps1"
function Read-Log([string]$Path){if(Test-Path -LiteralPath $Path){return [string](Get-Content -Raw -LiteralPath $Path)};return ''}
function Same-Process($Process,[string]$Created,[string]$Image){
 if(!$Process){return $false}
 return $Process.StartTime.ToFileTimeUtc().ToString() -eq $Created -and [IO.Path]::GetFullPath($Process.Path) -ieq [IO.Path]::GetFullPath($Image)
}
function Get-State($State){
 $process=Get-Process -Id $State.publisherPid -ErrorAction SilentlyContinue
 if($process -and !(Same-Process $process $State.publisherCreated $State.producer)){throw 'Publisher identity changed; refusing another process'}
 if((Get-FileHash -LiteralPath $State.producer).Hash.ToLower() -ne $State.producerSha256){throw 'Publisher image changed'}
 $engine=Get-Process -Id $State.enginePid -ErrorAction SilentlyContinue
 $engineAlive=$engine -and $engine.StartTime.ToFileTimeUtc().ToString() -eq $State.engineCreated
 $log=Read-Log $State.captureLog
 $restored=$log -match 'HOOK_STOP restored=1 .*entries=6'
 $failed=$log -match '(?m)^FAILED |(?m)^STOP .*failed=1|(?m)^HOOK_STOP restored=0'
 $nativeSeen=$log -match 'NATIVE_MAT_READY'
 $unsupported=@($log -split "`r?`n"|Where-Object {$_ -match '^COMPONENT .*support=(missing|unsupported)'})
 $status=if($unsupported){'Unsupported'}elseif($failed -or !$engineAlive){'Faulted'}elseif(!$process){if($restored){'StoppedVerified'}else{'ExitedUnverified'}}elseif($nativeSeen){'NativeReady'}else{'WaitingForObjects'}
 [ordered]@{state=$status;publisherAlive=[bool]$process;engineIdentityRetained=[bool]$engineAlive;nativeCyclesObserved=[bool]$nativeSeen;
  hooksRestored=[bool]$restored;unsupportedComponents=$unsupported;hardwarePlaybackVerified=$false;producer=$State.producer;pipe=$State.pipe;captureLog=$State.captureLog}
}
if($Action -in @('Status','Stop')){
 if(!$Metadata){throw 'Exact capture metadata is required'}
 $state=Get-Content -Raw -LiteralPath $Metadata|ConvertFrom-Json
 if($state.schema -ne 'native-plugin-live-v1' -or $state.stopEvent -notlike 'Local\SunshineMatLiveStop-*' -or $state.pipe -notlike '\\.\pipe\SunshineMatLive-*'){throw 'Invalid live capture metadata'}
 $status=Get-State $state
 if($Action -eq 'Status'){$status|ConvertTo-Json;return}
 if($status.publisherAlive){
  $process=Get-Process -Id $state.publisherPid
  if(!(Same-Process $process $state.publisherCreated $state.producer)){throw 'Publisher changed before stop'}
  $stopSignal=[Threading.EventWaitHandle]::OpenExisting($state.stopEvent)
  try{[void]$stopSignal.Set()}finally{$stopSignal.Dispose()}
  if(!$process.WaitForExit(15000)){throw 'Native stop exceeded 15 seconds; no force kill/unload; inspect capture log'}
 }
 $status=Get-State $state
 $status|ConvertTo-Json
 if($status.state -ne 'StoppedVerified'){throw 'Capture stop/restoration not verified; inspect original log'}
 return
}
if(!$Build -or !$EnginePid -or !$EngineCreated){throw 'Reviewed build and exact engine PID/creation FILETIME are required'}
if($Action -eq 'Start' -and (!$Output -or ($Continuous -eq ($Seconds -gt 0)))){throw 'Start needs fresh Output and exactly one of -Continuous or -Seconds 1..60'}
if($Action -eq 'Preflight' -and ($Continuous -or $Seconds -or $Reattach -or $SessionId)){throw 'Capture options require explicit -Action Start'}
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$manifest=Get-Content -Raw -LiteralPath (Join-Path $buildRoot build.json)|ConvertFrom-Json
# Source/test hashes are build provenance, not launch-time dependencies.
Assert-MatBuildArtifacts $buildRoot $manifest
$trusted=@{}
foreach($name in @('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll')){$trusted[$name]=Get-MatTrustedHash $manifest $name}
function Invoke-Preflight([string]$Image){
 $result=& $Image --preflight $EnginePid $EngineCreated 2>&1
 if($LASTEXITCODE -eq 3){
  $components=(@($result)|Where-Object {"$_" -like 'COMPONENT *'}) -join "`n"
  throw "This Windows audio engine or Dolby Access build has not been verified for MAT capture; no attach/elevation attempted. Include these lines when reporting it:`n$components"
 }
 if($LASTEXITCODE){throw "Read-only plugin preflight failed; no attach/elevation attempted: $result"}
 return $result
}
if($Action -eq 'Preflight'){Invoke-Preflight (Join-Path $buildRoot sunshine-plugin-mat-capture.exe);return}
if(Test-Path -LiteralPath $Output){throw 'Fresh output directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$exeHash=$trusted['sunshine-plugin-mat-capture.exe']
$programRoot=[Environment]::GetFolderPath('ProgramFiles')
Assert-MatDeploymentEntry $programRoot
$deploymentRoot=Join-Path $programRoot 'SunshineSonicMatLive'
Initialize-MatDeploymentDirectory $deploymentRoot
$deployment=Join-Path $deploymentRoot $exeHash
Initialize-MatDeploymentDirectory $deployment
$deployed=@{}
foreach($name in @('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll')){
 $from=Join-Path $buildRoot $name;$hash=$trusted[$name]
 $targetName=if($name.EndsWith('.dll')){[IO.Path]::GetFileNameWithoutExtension($name)+'-'+$hash.Substring(0,16)+'.dll'}else{$name}
 $to=Join-Path $deployment $targetName
 if(Test-Path -LiteralPath $to){Assert-MatDeploymentEntry $to}
 Copy-MatDeploymentFile $from $to $hash
 Assert-MatDeploymentEntry $to
 $deployed[$name]=$to
}
$producer=$deployed['sunshine-plugin-mat-capture.exe'];$tap=$deployed['sunshine-plugin-object-tap.dll'];$codec=$deployed['sunshine-sonic-mat-worker.dll']
Get-ChildItem -LiteralPath $deployment -Force|ForEach-Object {Assert-MatDeploymentEntry $_.FullName}
# Run the elevated preflight only from the verified, protected copy.
Invoke-Preflight $producer|Set-Content -LiteralPath (Join-Path $out preflight.log)
$id=if($SessionId){$SessionId}else{[guid]::NewGuid().ToString('N')};$pipe='\\.\pipe\SunshineMatLive-active';$stop="Local\SunshineMatLiveStop-$id"
foreach($value in @($producer,$tap,$codec,$out)){if($value.Contains('"') -or $value.Contains("`r") -or $value.Contains("`n")){throw 'Invalid launch path'}}
$mode=if($Reattach){'--reattach'}else{'--attach'}
$arguments=@($mode,"$EnginePid","$EngineCreated",('"'+$tap+'"'),$trusted['sunshine-plugin-object-tap.dll'],('"'+$codec+'"'),$trusted['sunshine-sonic-mat-worker.dll'],$pipe,$stop)
if($Continuous){$arguments+='--continuous'}else{$arguments+=@('--max-seconds',"$Seconds")}
$process=$null;$complete=$false
try{
 $process=Start-Process -FilePath $producer -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/capture.log" -RedirectStandardError "$out/capture-errors.log"
 $state=@{schema='native-plugin-live-v1';publisherPid=$process.Id;publisherCreated=$process.StartTime.ToFileTimeUtc().ToString();producer=$producer;producerSha256=$exeHash;
  enginePid=$EnginePid;engineCreated=$EngineCreated.ToString();pipe=$pipe;stopEvent=$stop;captureLog="$out/capture.log";continuous=[bool]$Continuous;maximumSeconds=$Seconds;sampleReplayUsed=$false}
 $state|ConvertTo-Json|Set-Content -LiteralPath "$out/capture.json"
 $deadline=[DateTime]::UtcNow.AddSeconds(3)
 while((Read-Log "$out/capture.log") -notmatch 'ATTACHED source=plugin-objects-to-native-MAT'){
  if($process.WaitForExit(20) -or [DateTime]::UtcNow -ge $deadline){throw 'Plugin attach failed/timed out; inspect capture log; no retry'}
 }
 Get-State $state|ConvertTo-Json
 Write-Host "Metadata: $out/capture.json"
 Write-Host 'On first use, enable MAT in Sunshine, then save and apply (restart Sunshine). Once applied, capture connects automatically; no parameters need copying.'
 $complete=$true
}finally{
 if(!$complete -and $process -and !$process.HasExited){
  try{$stopSignal=[Threading.EventWaitHandle]::OpenExisting($stop);try{[void]$stopSignal.Set()}finally{$stopSignal.Dispose()}}catch{Write-Warning "Unable to signal owned capture: $_"}
  if(!$process.WaitForExit(15000)){Write-Warning 'Capture remains active; preserve metadata; no forced kill/unload'}
 }
}
