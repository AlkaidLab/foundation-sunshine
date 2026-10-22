param(
 [Parameter(Mandatory=$true)][string]$Manifest,
 [Parameter(Mandatory=$true)][string]$Output,
 [Parameter(Mandatory=$true)][uint32]$EnginePid,
 [Parameter(Mandatory=$true)][uint64]$EngineCreated,
 [Parameter(Mandatory=$true)][string]$EndpointId,
 [ValidateRange(8,30)][int]$Seconds=20,
 [int]$Port=52410,
 [switch]$Reattach,[switch]$OperatorControl
)
# One explicitly authorized native run. No elevation, automatic retry, endpoint
# selection/change, driver/service configuration, or recorded media source.
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh full-native output required'}
$m=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
foreach($f in $m.pins){if((Get-FileHash -LiteralPath $f.path).Hash.ToLower() -ne $f.sha256){throw "Reviewed file changed: $($f.path)"}}
$engine=Get-Process -Id $EnginePid
if($engine.StartTime.ToFileTimeUtc() -ne $EngineCreated -or $engine.Path -ine "$env:SystemRoot/System32/audiodg.exe".Replace('/','\')){throw 'Pinned system engine changed'}
$build=$m.nativeBuild;$native=Get-Content "$build/build.json" -Raw|ConvertFrom-Json
foreach($f in $native.sources){if((Get-FileHash -LiteralPath $f.path).Hash.ToLower() -ne $f.sha256){throw "Native source changed: $($f.path)"}}
foreach($f in $native.binaries){if((Get-FileHash -LiteralPath "$build/$($f.name)").Hash.ToLower() -ne $f.sha256){throw "Native binary changed: $($f.name)"}}
$exeHash=(Get-FileHash "$build/sunshine-plugin-mat-capture.exe").Hash.ToLower()
$deployment=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) "SunshineSonicMatLive/$exeHash"
New-Item -ItemType Directory -Path $deployment -Force|Out-Null
$deployed=@{}
foreach($name in @('sunshine-plugin-mat-capture.exe','sunshine-plugin-object-tap.dll','sunshine-sonic-mat-worker.dll')){
 $from=Join-Path $build $name;$hash=(Get-FileHash $from).Hash.ToLower()
 $targetName=if($name.EndsWith('.dll')){[IO.Path]::GetFileNameWithoutExtension($name)+'-'+$hash.Substring(0,16)+'.dll'}else{$name}
 $to=Join-Path $deployment $targetName
 if(Test-Path -LiteralPath $to){if((Get-FileHash $to).Hash.ToLower() -ne $hash){throw 'Immutable deployment differs'}}else{Copy-Item -LiteralPath $from -Destination $to}
 $deployed[$name]=$to
}
$publisher=$deployed['sunshine-plugin-mat-capture.exe'];$tap=$deployed['sunshine-plugin-object-tap.dll'];$codec=$deployed['sunshine-sonic-mat-worker.dll']
$arguments=@($m.harness,'--output',$Output,'--host-build',$m.hostBuild,'--client-build',$m.clientBuild,'--publisher',$publisher,
 '--tap',$tap,'--tap-sha',(Get-FileHash $tap).Hash.ToLower(),'--codec',$codec,'--codec-sha',(Get-FileHash $codec).Hash.ToLower(),
 '--probe',"$build/spatial-probe.exe",'--engine-pid',"$EnginePid",'--engine-created',"$EngineCreated",'--endpoint-id',$EndpointId,'--seconds',"$Seconds",'--port',"$Port")
if($Reattach){$arguments+='--reattach'}
if($OperatorControl){$arguments+=@('--operator-build',$build,'--powershell',(Get-Process -Id $PID).Path)}
& $m.python @arguments
$code=$LASTEXITCODE
$resultPath=Join-Path $Output 'result.json'
if(Test-Path -LiteralPath $resultPath){
 $result=Get-Content -LiteralPath $resultPath -Raw|ConvertFrom-Json
 $after=Get-Process -Id $EnginePid -ErrorAction SilentlyContinue
 $retained=$after -and $after.StartTime.ToFileTimeUtc() -eq $EngineCreated
 $restored=[string](Get-Content -LiteralPath (Join-Path $Output 'capture.log') -Raw -ErrorAction SilentlyContinue) -match 'HOOK_STOP restored=1 .*entries=6'
 $result|Add-Member -NotePropertyName engineIdentityRetained -NotePropertyValue ([bool]$retained) -Force
 $result|Add-Member -NotePropertyName hooksRestored -NotePropertyValue ([bool]$restored) -Force
 if(!$retained -or !$restored -or $code){$result.passed=$false}
 $result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $resultPath
 if(!$result.passed){throw 'Full native RTSP/video test failed; inspect original logs; no automatic retry'}
}else{throw 'Full native result absent; no automatic retry'}
