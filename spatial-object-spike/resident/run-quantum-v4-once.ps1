param(
 [string]$Build="$PSScriptRoot/quantum-v4-build06",
 [string]$Output="$PSScriptRoot/quantum-v4-native01"
)
# One explicitly approved UAC / capture attempt. Never elevate or retry here.
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Approved elevated launcher required'}
if(Test-Path -LiteralPath $Output){throw 'Output must be new'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$collector=$null;$emitter=$null;$engine=$null;$failure=$null;$completed=$false
$exe=(Resolve-Path -LiteralPath "$Build/quantum-v4-collector.exe").Path
function Save-Json($Name,$Data){$Data | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $out $Name) -Encoding utf8}
function Check-Hash($Path,$Expected){if((Get-FileHash -LiteralPath $Path).Hash -ne $Expected){throw "Hash mismatch: $Path"}}
try {
 $manifest=Get-Content -LiteralPath "$Build/build.json" -Raw | ConvertFrom-Json
 foreach($source in $manifest.source_files){Check-Hash "$PSScriptRoot/$($source.file)" $source.sha256}
 foreach($runtime in $manifest.runtime){Check-Hash "$Build/$($runtime.file)" $runtime.sha256}
 $dll=(Resolve-Path -LiteralPath "$Build/engine_tap_quantum_v4.dll").Path
 Check-Hash $dll $manifest.dll_sha256
 Check-Hash $exe $manifest.collector_sha256
 Check-Hash "$Build/quantum-v4-verify.exe" $manifest.verifier_sha256
 Check-Hash "$Build/quantum-v4-bridge-test.exe" $manifest.bridge_test_sha256
 Check-Hash 'C:/Windows/System32/audioeng.dll' '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3'
 $sourceExe=(Resolve-Path -LiteralPath "$PSScriptRoot/../spatial_probe.exe").Path
 Check-Hash $sourceExe 'DB7A536ACBBD82180FBE52D036B12B279EE0F13C60C5014535ED5E2D21FD4EAC'
 if(@(Get-Process collector,quantum-collector,quantum-v4-collector,spatial_probe -ErrorAction SilentlyContinue).Count){throw 'Another collector or controlled source is running'}
 $audit=& "$PSScriptRoot/session_audit.exe";$auditCode=$LASTEXITCODE
 $audit | Set-Content -LiteralPath "$out/endpoints-before.txt" -Encoding utf8
 if($auditCode -or -not($audit -match 'ENDPOINT index=1 default=1 .*id=\{0\.0\.0\.00000000\}\.\{c701c0a8-9e5c-4a66-a432-c33de86ba247\}')){throw 'Default endpoint changed; no endpoint changes allowed'}
 $capabilities=& $sourceExe --list;$capCode=$LASTEXITCODE
 $capabilities | Set-Content -LiteralPath "$out/capabilities.txt" -Encoding utf8
 if($capCode -or ($capabilities -join "`n") -notmatch '(?s)\[1\] F32D80U[^\n]*\n[^\n]*maxDynamic=20[^\n]*\n[^\n]*1ch 48000Hz 32bit tag=0x0003'){throw 'Spatial format/capacity changed'}
 $engines=@(Get-Process audiodg -ErrorAction SilentlyContinue)
 if($engines.Count -ne 1){throw 'Expected exactly one current audio engine; no restart or automatic reattach'}
 $engine=$engines[0];$heldEngine=$engine.Handle;$enginePid=$engine.Id
 $deployment='C:/ProgramData/SpatialObjectSpike'
 if(-not(Test-Path -LiteralPath $deployment)){throw 'Expected existing deployment directory'}
 $deployed=Join-Path $deployment 'engine_tap_quantum_v4.dll'
 if(Test-Path -LiteralPath $deployed){Check-Hash $deployed $manifest.dll_sha256}
 else {Copy-Item -LiteralPath $dll -Destination $deployed;Check-Hash $deployed $manifest.dll_sha256}
 Save-Json 'launch.json' ([ordered]@{approved_scope='One UAC, one 10-second native v4 retest';started=(Get-Date).ToString('o');audio_seconds_limit=10
  build=(Resolve-Path -LiteralPath $Build).Path;helper=$deployed;helper_sha256=$manifest.dll_sha256;collector_sha256=$manifest.collector_sha256
  audiodg_pid_before=$enginePid;endpoint_index=1;endpoint_id='{0.0.0.00000000}.{c701c0a8-9e5c-4a66-a432-c33de86ba247}'
  source_phases=@(@{objects=2;seconds=3;variant=0},@{objects=1;seconds=2;variant=1});warmup_audio=$false
  endpoint_changes=$false;driver_changes=$false;security_changes=$false;game_started=$false;automatic_retry=$false
  warning='Experimental hooks; helper pinned until engine naturally exits. No MAT encoding or Android proof.'})
 $capture=Join-Path $out 'capture'
 $collector=Start-Process -FilePath $exe -ArgumentList @('--output',"`"$capture`"",'--seconds','10') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/collector.stdout.txt" -RedirectStandardError "$out/collector.stderr.txt"
 $heldCollector=$collector.Handle;$clock=[Diagnostics.Stopwatch]::StartNew()
 while(-not(Test-Path -LiteralPath "$capture/ready.json")){
  if($collector.WaitForExit(50)){throw "Collector failed before READY: $($collector.ExitCode)"}
  if($clock.Elapsed.TotalSeconds -gt 4){throw 'READY timeout; no retry'}
 }
 $ready=Get-Content -LiteralPath "$capture/ready.json" -Raw | ConvertFrom-Json
 if($ready.audiodg_pid -ne $enginePid){throw 'Engine changed between audit and capture'}
 $phase=0;$phaseResults=@()
 foreach($settings in @(@(2,3,0),@(1,2,1))){
  if($collector.HasExited){throw 'Collector ended before source phase'}
  if($clock.Elapsed.TotalSeconds + $settings[1] + 1 -gt 9.5){throw 'Insufficient authorized capture time; no retry'}
  ++$phase
  $emitter=Start-Process -FilePath $sourceExe -ArgumentList @('--emit','--dev','1','--objects',"$($settings[0])",'--seconds',"$($settings[1])",'--variant',"$($settings[2])") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/source-$phase.stdout.txt" -RedirectStandardError "$out/source-$phase.stderr.txt"
  $heldEmitter=$emitter.Handle
  $remaining=[Math]::Max(1,[int]((9.5-$clock.Elapsed.TotalSeconds)*1000))
  if(-not $emitter.WaitForExit($remaining)){throw 'Controlled source timeout; no retry'}
  $emitter.Refresh();$phaseResults+=@{phase=$phase;pid=$emitter.Id;exit=$emitter.ExitCode;objects=$settings[0];requested_seconds=$settings[1];variant=$settings[2]}
  Save-Json 'source-exits.json' $phaseResults
  if($emitter.ExitCode -ne 0){throw "Source phase $phase failed: $($emitter.ExitCode)"}
  $emitter=$null
  if($phase -eq 1){Start-Sleep -Milliseconds 300}
 }
 if(-not $collector.WaitForExit(15000)){throw 'Collector stop timeout; cooperative stop only'}
 $collector.Refresh()
 if($collector.ExitCode -ne 0){throw "Collector failed: $($collector.ExitCode)"}
 $report=Get-Content -LiteralPath "$capture/summary.json" -Raw | ConvertFrom-Json
 if($report.status -ne 2 -or $report.error -ne 0 -or -not $report.original_bytes_restored -or $report.invalid -or $report.dropped_full -or $report.dropped_busy -or $report.unbound_after_arming){throw 'Native capture integrity/restoration failed'}
 $completed=$true
} catch {
 $failure=$_.Exception.Message
 $failure | Set-Content -LiteralPath "$out/launcher-error.txt" -Encoding utf8
} finally {
 # Never kill, restart, or change audiodg. Only our own source may be stopped.
 if($emitter -and -not $emitter.HasExited){Stop-Process -Id $emitter.Id -ErrorAction SilentlyContinue}
 if($collector -and -not $collector.HasExited){
  & $exe --stop $collector.Id | Set-Content -LiteralPath "$out/cooperative-stop.txt" -Encoding utf8
  $null=$collector.WaitForExit(10000)
 }
 Save-Json 'exit.json' ([ordered]@{completed=$completed;error=$failure;collector_pid=$(if($collector){$collector.Id}else{$null})
  collector_exited=$(if($collector){$collector.HasExited}else{$true});collector_exit=$(if($collector -and $collector.HasExited){$collector.ExitCode}else{$null})
  audiodg_pids_after=@((Get-Process audiodg -ErrorAction SilentlyContinue).Id)
  audited_engine_exited=$(if($engine){$engine.HasExited}else{$null});audited_engine_exit_code=$(if($engine -and $engine.HasExited){$engine.ExitCode}else{$null})
  finished=(Get-Date).ToString('o');mat_verified=$false;no_automatic_retry=$true})
}
if(-not $completed){exit 1}
