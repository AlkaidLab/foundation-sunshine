param(
 [Parameter(Mandatory=$true)][string]$Build,
 [Parameter(Mandatory=$true)][string]$Output,
 [Parameter(Mandatory=$true)][uint32]$EnginePid,
 [Parameter(Mandatory=$true)][uint64]$EngineCreated,
 [switch]$Attach
)
$ErrorActionPreference='Stop'
# Explicit operator entry point. Never elevates itself, enables a privilege,
# chooses another engine, changes an endpoint, retries an attach or restarts anything.
if(Test-Path -LiteralPath $Output){throw 'Use a fresh capture metadata directory'}
$buildRoot=(Resolve-Path -LiteralPath $Build).Path
$out=(New-Item -ItemType Directory -Path $Output).FullName
$exe=Join-Path $buildRoot 'sunshine-mat-capture.exe';$tap=Join-Path $buildRoot 'sunshine-mat-tap.dll'
foreach($file in @($exe,$tap)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing reviewed binary: $file"}}
& $exe --preflight $EnginePid $EngineCreated *> "$out/preflight.log"
if($LASTEXITCODE){throw "Windows refused the read-only pinned-engine preflight. See $out/preflight.log. No attach or elevation was attempted."}
if(!$Attach){Write-Host 'Preflight passed; native MAT readiness remains unknown. Explicit -Attach is required to capture.';return}
$tapHash=(Get-FileHash -LiteralPath $tap).Hash.ToLower();$exeHash=(Get-FileHash -LiteralPath $exe).Hash.ToLower()
# audiodg's service identity must be able to read the DLL; keep immutable versions
# in a normal ProgramData directory rather than widening a private user profile ACL.
$deployment=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) "SunshineMatLive/$tapHash"
New-Item -ItemType Directory -Path $deployment -Force | Out-Null
foreach($file in @($exe,$tap)){
 $target=Join-Path $deployment ([IO.Path]::GetFileName($file))
 if(Test-Path -LiteralPath $target){if((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file).Hash){throw "Existing deployment differs: $target"}}
 else {Copy-Item -LiteralPath $file -Destination $target}
}
$producer=Join-Path $deployment 'sunshine-mat-capture.exe';$deployedTap=Join-Path $deployment 'sunshine-mat-tap.dll'
$identity=[guid]::NewGuid().ToString('N');$pipe="\\.\pipe\SunshineMatLive-$identity";$stopEvent="Local\SunshineMatLiveStop-$identity"
# Start-Process on Windows joins argument strings. These paths cannot contain
# quotes/newlines; quote each path explicitly and never pass through a shell.
foreach($value in @($producer,$deployedTap,$pipe,$stopEvent)){if($value.Contains('"') -or $value.Contains("`r") -or $value.Contains("`n")){throw 'Invalid launch path/name'}}
$arguments=@('--attach',"$EnginePid","$EngineCreated",('"'+$deployedTap+'"'),$tapHash,$pipe,$stopEvent)
$process=Start-Process -FilePath $producer -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/capture.log" -RedirectStandardError "$out/capture-errors.log"
@{publisherPid=$process.Id;publisherCreated=$process.StartTime.ToUniversalTime().ToFileTimeUtc();producer=$producer;producerSha256=$exeHash;tap=$deployedTap;tapSha256=$tapHash;
 enginePid=$EnginePid;engineCreated=$EngineCreated;pipe=$pipe;stopEvent=$stopEvent;nativeMATReady='unverified; inspect capture output and negotiate actual cycles'} | ConvertTo-Json | Set-Content -LiteralPath "$out/capture.json" -Encoding utf8
Write-Host "Capture launch requested once. Metadata: $out/capture.json"
Write-Host "mat_live_pipe = $pipe"
Write-Host "mat_live_producer = $producer"
Write-Host 'A running publisher alone does not prove native MAT readiness. No installed Sunshine config was changed.'
