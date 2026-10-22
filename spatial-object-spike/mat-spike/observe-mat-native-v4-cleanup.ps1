# Read-only post-test observation. No elevation, native calls, unload or restart.
$ErrorActionPreference='Stop'
$root=Join-Path $PSScriptRoot 'mat-native-v4-run01'
$output=Join-Path $root 'cleanup-observation.json'
if(Test-Path -LiteralPath $output){throw 'Observation output must be new'}
$launch=Get-Content -LiteralPath (Join-Path $root 'launch.json') -Raw | ConvertFrom-Json
$exitInfo=Get-Content -LiteralPath (Join-Path $root 'exit.json') -Raw | ConvertFrom-Json
$engines=@(Get-Process audiodg -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,@{Name='StartTime';Expression={$_.StartTime.ToString('o')}})
$probe=@(Get-Process mat-replay-v4-probe -ErrorAction SilentlyContinue | Select-Object Id,ProcessName)
$launcher=Get-Process -Id 14036 -ErrorAction SilentlyContinue
$launcherStillSame=($null -ne $launcher -and $launcher.ProcessName -eq 'powershell')
$query='completed';$events=@()
try{
 $events=@(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1001,1002;StartTime=([datetime]$launch.started).AddSeconds(-5)} -ErrorAction Stop |
   Where-Object {$_.Message -match 'audiodg|mat-replay-v4|mat_replay_v4'} |
   Select-Object @{Name='Time';Expression={$_.TimeCreated.ToString('o')}},Id,ProviderName,Message)
}catch{
 if($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*'){$query='completed_no_matching_event_ids'}else{$query='unavailable: '+$_.Exception.Message}
}
$result=[ordered]@{
 observed=(Get-Date).ToString('o');scope='Read-only post-test observation; no forced termination/unload/restart'
 audited_engine_pid=$launch.audiodg_pid_before;audiodg=$engines
 audited_engine_still_present=(@($engines | Where-Object {$_.Id -eq $launch.audiodg_pid_before}).Count -eq 1)
 original_probe_pid=$exitInfo.probe_pid;probe_processes=$probe;launcher_pid=14036;launcher_still_present=$launcherStillSame
 services=@(Get-Service Audiosrv,AudioEndpointBuilder | Select-Object Name,@{Name='Status';Expression={$_.Status.ToString()}})
 application_event_query=$query;relevant_application_fault_events=$events
 helper_path=$launch.helper;helper_sha256=(Get-FileHash -LiteralPath $launch.helper).Hash
 helper_pinned_until_natural_engine_exit=$true;helper_deleted=$false
 endpoint_changed=$false;driver_changed=$false;security_changed=$false;additional_native_attempts=0
 caveat='Point-in-time observation; no matching Application events alone is not a universal health proof.'
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding utf8
$result | ConvertTo-Json -Depth 8
