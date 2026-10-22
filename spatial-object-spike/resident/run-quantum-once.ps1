param(
 [string]$Build="$PSScriptRoot/quantum-build02",
 [string]$Output="$PSScriptRoot/quantum-native01"
)
# Run only by the user-approved one-shot UAC launch. No elevation/retry here.
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Only this approved elevated short test may execute'}
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$collector=$null;$emitter=$null;$failure=$null;$completed=$false
$exe=(Resolve-Path -LiteralPath "$Build/quantum-collector.exe").Path
function Save-Json($Name,$Data){$Data | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out $Name) -Encoding utf8}
try {
 $manifest=Get-Content -LiteralPath "$Build/build.json" -Raw | ConvertFrom-Json
 foreach($source in $manifest.source_files){if((Get-FileHash -LiteralPath "$PSScriptRoot/$($source.file)").Hash -ne $source.sha256){throw "Source changed since tested build: $($source.file)"}}
 $dll=(Resolve-Path -LiteralPath "$Build/engine_tap_quantum_v1.dll").Path
 if((Get-FileHash -LiteralPath $dll).Hash -ne $manifest.dll_sha256 -or (Get-FileHash -LiteralPath $exe).Hash -ne $manifest.collector_sha256){throw 'Tested probe binary hash mismatch'}
 if((Get-FileHash -LiteralPath 'C:/Windows/System32/audioeng.dll').Hash -ne '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3'){throw 'Unverified audioeng version'}
 $sourceExe=(Resolve-Path -LiteralPath "$PSScriptRoot/../spatial_probe.exe").Path
 if((Get-FileHash -LiteralPath $sourceExe).Hash -ne 'DB7A536ACBBD82180FBE52D036B12B279EE0F13C60C5014535ED5E2D21FD4EAC'){throw 'Controlled source hash mismatch'}
 if(@(Get-Process collector,quantum-collector -ErrorAction SilentlyContinue).Count){throw 'Another collector is running; do not overlap hooks'}
 $engines=@(Get-Process audiodg -ErrorAction SilentlyContinue)
 if($engines.Count -ne 1 -or $engines[0].Id -ne 35640){throw 'Audited audiodg instance changed; no automatic reattachment'}
 $enginePid=$engines[0].Id
 $audit=& "$PSScriptRoot/session_audit.exe";$auditCode=$LASTEXITCODE
 $audit | Set-Content -LiteralPath "$out/endpoints-before.txt" -Encoding utf8
 if($auditCode -or -not($audit -match 'ENDPOINT index=1 default=1 .*id=\{0\.0\.0\.00000000\}\.\{c701c0a8-9e5c-4a66-a432-c33de86ba247\}')){throw 'Default endpoint changed; do not select another endpoint'}
 $capabilities=& $sourceExe --list;$capCode=$LASTEXITCODE
 $capabilities | Set-Content -LiteralPath "$out/capabilities.txt" -Encoding utf8
 $caps=$capabilities -join "`n"
 if($capCode -or $caps -notmatch '(?s)\[1\] F32D80U[^\n]*\n[^\n]*maxDynamic=20[^\n]*\n[^\n]*1ch 48000Hz 32bit tag=0x0003'){throw 'Audited spatial format/capacity changed'}
 $deployment='C:/ProgramData/SpatialObjectSpike'
 if(-not(Test-Path -LiteralPath $deployment)){throw 'Expected existing deployment directory'}
 $deployed=Join-Path $deployment 'engine_tap_quantum_v1.dll'
 if(-not(Test-Path -LiteralPath $deployed)){Copy-Item -LiteralPath $dll -Destination $deployed}
 if((Get-FileHash -LiteralPath $deployed).Hash -ne $manifest.dll_sha256){throw 'Pinned DLL mismatch; do not overwrite or restart audio engine'}
 Save-Json 'launch.json' ([ordered]@{approved_scope='One UAC, one 10-second native Process-boundary capture';audio_seconds_limit=10
  build=(Resolve-Path -LiteralPath $Build).Path;helper=$deployed;helper_sha256=$manifest.dll_sha256;collector_sha256=$manifest.collector_sha256
  audiodg_pid_before=$enginePid;endpoint_index=1;endpoint_id='{0.0.0.00000000}.{c701c0a8-9e5c-4a66-a432-c33de86ba247}'
  source_phases=@(@{objects=2;seconds=3;variant=0},@{objects=1;seconds=2;variant=1})
  endpoint_changes=$false;driver_changes=$false;security_changes=$false;game_started=$false;automatic_retry=$false
  warning='Experimental hooks restored on stop. Helper/trampoline pinned until audiodg naturally exits. No MAT encoder instance created.'})
 $capture=Join-Path $out 'capture'
 $collector=Start-Process -FilePath $exe -ArgumentList @('--output',"`"$capture`"",'--seconds','10','--backend','dolby-quantum','--max-mb','64','--chunk-mb','1') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/collector.stdout.txt" -RedirectStandardError "$out/collector.stderr.txt"
 $heldCollector=$collector.Handle;$clock=[Diagnostics.Stopwatch]::StartNew()
 $ready=Join-Path $capture 'ready.txt'
 while(-not(Test-Path -LiteralPath $ready)){
  if($collector.WaitForExit(50)){throw "Collector failed before READY: $($collector.ExitCode)"}
  if($clock.Elapsed.TotalSeconds -gt 4){throw 'READY timeout; stop without retry'}
 }
 $phase=0;$phaseResults=@()
 foreach($settings in @(@(2,3,0),@(1,2,1))){
  if($collector.HasExited){throw 'Collector ended before source phase'}
  if($clock.Elapsed.TotalSeconds -gt 6){throw 'Not enough time in the authorized 10-second capture'}
  ++$phase
  $emitter=Start-Process -FilePath $sourceExe -ArgumentList @('--emit','--dev','1','--objects',"$($settings[0])",'--seconds',"$($settings[1])",'--variant',"$($settings[2])") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/source-$phase.stdout.txt" -RedirectStandardError "$out/source-$phase.stderr.txt"
  $heldEmitter=$emitter.Handle
  $remaining=[Math]::Max(1,[int]((9.5-$clock.Elapsed.TotalSeconds)*1000))
  if(-not $emitter.WaitForExit($remaining)){throw 'Source timeout; capture will stop without another attempt'}
  $emitter.Refresh();$phaseResults+=@{phase=$phase;pid=$emitter.Id;exit=$emitter.ExitCode;objects=$settings[0];requested_seconds=$settings[1]}
  Save-Json 'source-exits.json' $phaseResults
  if($emitter.ExitCode -ne 0){throw "Source phase $phase failed: $($emitter.ExitCode)"}
  $emitter=$null
  if($phase -eq 1){Start-Sleep -Milliseconds 300}
 }
 if(-not $collector.WaitForExit(15000)){throw 'Collector did not stop normally; request cooperative stop only'}
 $collector.Refresh()
 if($collector.ExitCode -ne 0){throw "Collector failed: $($collector.ExitCode)"}
 $report=Get-Content -LiteralPath "$capture/summary.json" -Raw | ConvertFrom-Json
 if($report.status -ne 2 -or $report.error -ne 0 -or -not $report.original_bytes_restored -or $report.invalid -or $report.dropped_full -or $report.dropped_busy){throw 'Native capture integrity/restoration failed'}
 $completed=$true
} catch {
 $failure=$_.Exception.Message
 $failure | Set-Content -LiteralPath "$out/launcher-error.txt" -Encoding utf8
} finally {
 # Never kill/restart audiodg. Only this synthetic source may be forcibly stopped.
 if($emitter -and -not $emitter.HasExited){Stop-Process -Id $emitter.Id -ErrorAction SilentlyContinue}
 if($collector -and -not $collector.HasExited){
  & $exe --stop $collector.Id | Set-Content -LiteralPath "$out/cooperative-stop.txt" -Encoding utf8
  $null=$collector.WaitForExit(10000)
 }
 $after=@(Get-Process audiodg -ErrorAction SilentlyContinue)
 Save-Json 'exit.json' ([ordered]@{completed=$completed;error=$failure;collector_pid=$(if($collector){$collector.Id}else{$null})
  collector_exited=$(if($collector){$collector.HasExited}else{$true});collector_exit=$(if($collector -and $collector.HasExited){$collector.ExitCode}else{$null})
  audiodg_pids_after=@($after.Id);finished=(Get-Date).ToString('o');mat_verified=$false;no_automatic_retry=$true})
}
if(-not $completed){exit 1}
