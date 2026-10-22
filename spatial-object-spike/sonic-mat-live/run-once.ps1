param(
 [Parameter(Mandatory=$true)][string]$Build,
 [Parameter(Mandatory=$true)][string]$Checker,
 [Parameter(Mandatory=$true)][string]$Output,
 [Parameter(Mandatory=$true)][uint32]$EnginePid,
 [Parameter(Mandatory=$true)][uint64]$EngineCreated,
 [Parameter(Mandatory=$true)][string]$EndpointId,
 [switch]$Reattach,
 [ValidateSet('sonic','plugin')][string]$Backend='sonic'
)
# One explicitly initiated native test, no self-elevation, restart, endpoint change,
# privilege adjustment, recorded source or automatic retry. Source is generated
# LIVE through ISpatialAudioClient on the exact endpoint; not an audio recording.
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh native test output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$buildRoot=(Resolve-Path -LiteralPath $Build).Path;$checkExe=(Resolve-Path -LiteralPath $Checker).Path
$manifest=Get-Content "$buildRoot/build.json" -Raw | ConvertFrom-Json
foreach($file in $manifest.binaries){if((Get-FileHash -LiteralPath "$buildRoot/$($file.name)").Hash.ToLower() -ne $file.sha256){throw "Binary changed: $($file.name)"}}
foreach($file in $manifest.sources){if((Get-FileHash -LiteralPath $file.path).Hash.ToLower() -ne $file.sha256){throw "Source changed: $($file.path)"}}
$engine=Get-Process -Id $EnginePid
if($engine.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $EngineCreated -or [IO.Path]::GetFullPath($engine.Path) -ine [IO.Path]::GetFullPath("$env:SystemRoot/System32/audiodg.exe")){throw 'Pinned engine identity changed'}
$probe=Join-Path $buildRoot 'spatial-probe.exe'
& $probe --list --endpoint $EndpointId *> "$out/endpoint-before.log"
if($LASTEXITCODE){throw 'Chosen active endpoint not found; no reselection'}
$caps=Get-Content "$out/endpoint-before.log" -Raw
# The exact-endpoint source activation below is authoritative. Capacity alone
# never counts as proof that Sonic callbacks or native MAT were observed.
if($caps -notmatch [regex]::Escape($EndpointId)){throw 'Endpoint identity absent from audit'}
$captureName="sunshine-$Backend-mat-capture.exe";$tapName=if($Backend -eq 'sonic'){'sunshine-sonic-object-tap.dll'}else{'sunshine-plugin-object-tap.dll'}
$exe=Join-Path $buildRoot $captureName;$exeHash=(Get-FileHash $exe).Hash.ToLower()
$deployment=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) "SunshineSonicMatLive/$exeHash"
New-Item -ItemType Directory -Path $deployment -Force | Out-Null
$deployed=@{}
foreach($name in @($captureName,$tapName,'sunshine-sonic-mat-worker.dll')){
 $from=Join-Path $buildRoot $name;$hash=(Get-FileHash $from).Hash.ToLower();$targetName=if($name.EndsWith('.dll')){[IO.Path]::GetFileNameWithoutExtension($name)+'-'+$hash.Substring(0,16)+'.dll'}else{$name};$to=Join-Path $deployment $targetName
 if(Test-Path -LiteralPath $to){if((Get-FileHash $from).Hash -ne (Get-FileHash $to).Hash){throw 'Immutable deployment differs'}}else{Copy-Item -LiteralPath $from -Destination $to}
 $deployed[$name]=$to
}
$publisher=$deployed[$captureName];$tap=$deployed[$tapName];$codec=$deployed['sunshine-sonic-mat-worker.dll']
$tapHash=(Get-FileHash $tap).Hash.ToLower();$codecHash=(Get-FileHash $codec).Hash.ToLower()
$id=[guid]::NewGuid().ToString('N');$pipe="\\.\pipe\SunshineMatLive-$id";$stopName="Local\SunshineMatLiveStop-$id"
foreach($p in @($publisher,$tap,$codec,$probe,$checkExe,$out,$EndpointId)){if($p.Contains('"') -or $p.Contains("`r") -or $p.Contains("`n")){throw 'Invalid process argument'}}
@{scope='One native object capture + dedicated native MAT worker + loopback receive test';backend=$Backend;enginePid=$EnginePid;engineCreated="$EngineCreated";endpoint=$EndpointId;
 nativeCaptureSeconds=8;liveSourceSeconds=10;pipe=$pipe;producer=$publisher;producerSha256=$exeHash;tapSha256=$tapHash;codecSha256=$codecHash;checker=$checkExe;checkerSha256=(Get-FileHash $checkExe).Hash.ToLower();
 source='live ISpatialAudioClient two-object generator; no file input';endpointChange=$false;driverServiceChange=$false;automaticRetry=$false} | ConvertTo-Json -Depth 4 | Set-Content "$out/launch.json" -Encoding utf8
$capture=$null;$source=$null;$client=$null;$failure=$null;$watch=[Diagnostics.Stopwatch]::StartNew()
try {
 $mode=if($Reattach){'--reattach'}else{'--attach'}
 $arguments=@($mode,"$EnginePid","$EngineCreated",('"'+$tap+'"'),$tapHash,('"'+$codec+'"'),$codecHash,$pipe,$stopName,'--max-seconds','8')
 $capture=Start-Process -FilePath $publisher -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/capture.log" -RedirectStandardError "$out/capture-errors.log"
 while([string](Get-Content "$out/capture.log" -Raw) -notmatch ('ATTACHED source='+$Backend+'-objects-to-native-MAT')){
  if($capture.WaitForExit(20) -or $watch.Elapsed.TotalSeconds -gt 3){throw 'Sonic attach failed/timed out; no retry'}
 }
 $source=Start-Process -FilePath $probe -ArgumentList @('--emit','--endpoint',$EndpointId,'--seconds','10','--objects','2','--variant','0') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/source.log" -RedirectStandardError "$out/source-errors.log"
 while([string](Get-Content "$out/capture.log" -Raw) -notmatch 'NATIVE_MAT_READY'){
  if($capture.WaitForExit(20) -or $watch.Elapsed.TotalSeconds -gt 4){throw 'No actual native MAT from live Sonic source; no replay fallback'}
 }
 $engine.Modules | Select-Object ModuleName,FileName,BaseAddress,ModuleMemorySize | ConvertTo-Json | Set-Content "$out/engine-modules.json"
 $client=Start-Process -FilePath $checkExe -ArgumentList @('--pipe',$pipe,'--producer',('"'+$publisher+'"'),'--max-seconds','12','--drop-for-fec') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/receiver.log" -RedirectStandardError "$out/receiver-errors.log"
 if(!$capture.WaitForExit(12000)){throw 'Bounded native publisher did not exit'}
 if(!$client.WaitForExit(4000)){throw 'Loopback receiver did not exit'}
 if($capture.ExitCode -or $client.ExitCode){throw 'Native publisher/receiver failed; inspect capture and receiver logs'}
 if(!$source.WaitForExit(12000)){throw 'Live source did not finish its separately bounded 10-second emission'}
 if($source.ExitCode){throw 'Live object source failed; inspect source log'}
}catch{$failure=$_.Exception.Message}finally{
 if($capture -and !$capture.HasExited){try{$event=[Threading.EventWaitHandle]::OpenExisting($stopName);$event.Set()|Out-Null;$event.Dispose()}catch{if(!$failure){$failure=$_.Exception.Message}};$capture.WaitForExit(5000)|Out-Null}
 if($client -and !$client.HasExited){$client.WaitForExit(13000)|Out-Null}
 if($source -and !$source.HasExited){$source.WaitForExit(13000)|Out-Null}
 $engine.Modules | Select-Object ModuleName,FileName,BaseAddress,ModuleMemorySize | ConvertTo-Json | Set-Content "$out/engine-modules-after.json"
 $log=if(Test-Path "$out/capture.log"){Get-Content "$out/capture.log" -Raw}else{''}
 $received=if(Test-Path "$out/receiver.log"){Get-Content "$out/receiver.log" -Raw}else{''}
 $after=Get-Process -Id $EnginePid -ErrorAction SilentlyContinue
 @{passed=(!$failure -and $log -match 'NATIVE_MAT_READY' -and $log -match '(SONIC|PLUGIN)_STOP .*restored=1' -and $log -match 'HOOK_STOP restored=1' -and $log -match 'LEASE_END_ACK cycles=[1-9][0-9]*' -and $received -match 'PASS_NATIVE_LOOPBACK');failure=$failure;
  elapsedSeconds=$watch.Elapsed.TotalSeconds;nativeReady=($log -match 'NATIVE_MAT_READY');hooksRestored=($log -match 'HOOK_STOP restored=1');
  engineIdentityRetained=($after -and $after.StartTime.ToUniversalTime().ToFileTimeUtc() -eq $EngineCreated);publisherExited=(!$capture -or $capture.HasExited);
  publisherExitCode=$(if($capture -and $capture.HasExited){$capture.ExitCode}else{$null});sourceExitCode=$(if($source -and $source.HasExited){$source.ExitCode}else{$null});receiverExitCode=$(if($client -and $client.HasExited){$client.ExitCode}else{$null});
  sourceExited=(!$source -or $source.HasExited);receiverExited=(!$client -or $client.HasExited);sampleReplayUsed=$false;audioPayloadWrittenToDisk=$false;hardwareSinkAccepted=$false;fullSunshineRtspSession=$false} | ConvertTo-Json -Depth 4 | Set-Content "$out/result.json" -Encoding utf8
}
if($failure){throw $failure}
