param(
 [ValidateSet('Preflight','AuthorizedNative')][string]$Mode='Preflight',
 [string]$Build="$PSScriptRoot/mat-native-launcher-build04",
 [string]$InteropProof="$PSScriptRoot/mat-dll-interop-proof02",
 [string]$Bridge="$PSScriptRoot/cpp-native-v4-replay02",
 [string]$Output="$PSScriptRoot/mat-native-launcher-preflight01",
 [string]$Confirmation=''
)
# Default is read-only towards audio processes. This file never self-elevates.
# The native switch/string is NOT evidence of human authorization; obtain it
# separately for EACH attempt before invoking AuthorizedNative.
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh output directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
function Save-Json($Name,$Value){$Value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $out $Name) -Encoding utf8}
function Check-Hash($Path,$Expected){if((Get-FileHash -LiteralPath $Path).Hash -ne $Expected){throw "Frozen artifact changed: $Path"}}
$launched=$false;$completed=$false;$child=$null;$issue=$null
try{
 $buildManifest=Get-Content -LiteralPath "$Build/build.json" -Raw | ConvertFrom-Json
 foreach($entry in $buildManifest.source_files){Check-Hash "$PSScriptRoot/$($entry.file)" $entry.sha256}
 $exe=(Resolve-Path -LiteralPath "$Build/mat-native-launcher.exe").Path;Check-Hash $exe $buildManifest.executable_sha256
 $dll=$buildManifest.receiver_dll;Check-Hash $dll $buildManifest.receiver_dll_sha256
 Check-Hash (Join-Path (Split-Path -Parent $dll) 'build.json') $buildManifest.receiver_manifest_sha256
 $receiverManifest=Get-Content -LiteralPath (Join-Path (Split-Path -Parent $dll) 'build.json') -Raw | ConvertFrom-Json
 foreach($entry in $receiverManifest.source_files){Check-Hash "$PSScriptRoot/$($entry.file)" $entry.sha256}
 $ownProof=Get-Content -LiteralPath "$Build/selftest/proof.json" -Raw | ConvertFrom-Json
 $interop=Get-Content -LiteralPath "$InteropProof/proof.json" -Raw | ConvertFrom-Json
 if(-not $ownProof.passed -or $ownProof.selftests_passed -ne 15 -or -not $interop.passed -or $interop.receiver_dll_sha256 -ne $buildManifest.receiver_dll_sha256){throw 'Expected own-child and cross-process proofs'}
 $sourceHashes=@('aa345cc683f3daa98c680a9a25a73e04ec3272a74f86c028a535ae347fb729a8','1ff71d6cd0e27d8ecbdc5af6adf769df51001fdd0f99d449f49c67bec360fafc')
 foreach($domain in 1,2){
  $source=(Resolve-Path -LiteralPath "$Bridge/domain-$domain-mat-object-input.bin").Path;Check-Hash $source $sourceHashes[$domain-1]
  & $exe --validate-source $source "$out/source-$domain" | Set-Content -LiteralPath "$out/source-$domain.stdout.txt" -Encoding utf8
  if($LASTEXITCODE){throw "Source $domain validation rejected"}
  $validated=Get-Content -LiteralPath "$out/source-$domain/source.json" -Raw | ConvertFrom-Json
  if($validated.source_samples -ne @('288000','96000')[$domain-1] -or $validated.source_rounds -ne @(306,206)[$domain-1]){throw 'Unexpected bounded source dimensions'}
 }
 & $exe --inspect "$out/inspection" | Set-Content -LiteralPath "$out/inspection.stdout.txt" -Encoding utf8
 if($LASTEXITCODE){throw 'Read-only inspection failed'}
 $inspection=Get-Content -LiteralPath "$out/inspection/inspection.json" -Raw | ConvertFrom-Json
 $busy=@(Get-Process collector,quantum-collector,quantum-v4-collector,mat-lifecycle-probe,mat-encode-probe,mat-replay-v4-probe,mat-native-launcher,mat-dll-producer,mat-dll-host -ErrorAction SilentlyContinue)
 $deployment='C:/ProgramData/SpatialObjectSpike';$staged=Join-Path $deployment 'mat_ipc_receiver.dll'
 $stagingNeeded=-not(Test-Path -LiteralPath $staged)
 if(-not $stagingNeeded){Check-Hash $staged $buildManifest.receiver_dll_sha256}
 $blockers=@();if(-not $inspection.component_hashes_match){$blockers+='Installed component hashes do not match frozen native ABI'}
 if(-not $inspection.exactly_one_existing_engine){$blockers+='Exactly one existing audiodg required; do not start/restart engine automatically'}
 if(-not $inspection.current_process_elevated){$blockers+='A fresh separately approved elevated process is required for native execution'}
 if($busy.Count){$blockers+='Another probe/test process is running'}
 if(-not(Test-Path -LiteralPath $deployment -PathType Container)){$blockers+='Existing staging directory required; no automatic ACL/directory changes'}
 Save-Json 'preflight.json' ([ordered]@{preflight_completed=$true;ready_for_authorized_attempt=($blockers.Count -eq 0);blockers=$blockers;mode=$Mode
  executable=$exe;executable_sha256=$buildManifest.executable_sha256;receiver_dll_sha256=$buildManifest.receiver_dll_sha256
  launcher_script_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;own_test_proof_sha256=(Get-FileHash -LiteralPath "$Build/selftest/proof.json").Hash
  interop_proof_sha256=(Get-FileHash -LiteralPath "$InteropProof/proof.json").Hash;audiodg_pids=$inspection.audiodg_pids;component_hashes_match=$inspection.component_hashes_match
  staging_needed=$stagingNeeded;staging_performed=$false;audiodg_opened=$false;native_encoder_called=$false;security_changed=$false;endpoint_changed=$false;engine_started=$false
  planned_source_domain=1;planned_source_rounds=306;planned_Process_rounds=356;planned_Source_samples=288000;planned_input_seconds=3.56
  warning='Preflight completed does NOT mean native-ready or encoded. Prior UAC approvals cannot be reused.'})
 if($Mode -eq 'Preflight'){$completed=$true}
 else{
  if($Confirmation -cne 'FRESH_APPROVAL_ONE_DOMAIN_NO_RETRY'){throw 'Fresh human approval required; marker alone does not supply authorization'}
  if($blockers.Count){throw ('Native attempt blocked: '+($blockers -join '; '))}
  # Select one existing engine ONCE and freeze its exact creation time. Never
  # retarget/retry if this engine exits. The native executable repeats checks.
  $engine=Get-Process -Id $inspection.audiodg_pids[0]
  $engineCreated=$engine.StartTime.ToUniversalTime().ToFileTimeUtc()
  if($stagingNeeded){Copy-Item -LiteralPath $dll -Destination $staged;Check-Hash $staged $buildManifest.receiver_dll_sha256}
  $source=(Resolve-Path -LiteralPath "$Bridge/domain-1-mat-object-input.bin").Path
  $epoch=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();$capture=Join-Path $out 'capture'
  Save-Json 'launch.json' ([ordered]@{scope='One explicitly approved existing-audiodg domain-1 replay, one attempt, no retries';engine_pid=$engine.Id;engine_creation_time=$engineCreated
   helper=$staged;helper_sha256=$buildManifest.receiver_dll_sha256;helper_staged_now=$stagingNeeded;executable_sha256=$buildManifest.executable_sha256;source_sha256=$sourceHashes[0];epoch=$epoch
   source_rounds=306;warm_rounds=20;drain_rounds=30;native_deadline_seconds=10;endpoint_changes=$false;drivers=$false;hooks=$false;privileges_adjusted=$false
   retained_helper_warning='Helper/Windows runtime and vendor references can remain until natural host exit. No unload or forced termination.'})
  $arguments=@('--authorized-native-once',"`"$staged`"","`"$source`"",'1',"$epoch","$($engine.Id)","$engineCreated","`"$capture`"",'NO_RETRY_NO_ENDPOINT_CHANGES')
  $child=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/native.stdout.txt" -RedirectStandardError "$out/native.stderr.txt"
  $launched=$true;$heldChild=$child.Handle
  if(-not $child.WaitForExit(30000)){throw 'Native launcher exceeded bound; no force kill, unload, or retry'}
  $child.Refresh();if($child.ExitCode -ne 0){throw "Native attempt exited $($child.ExitCode); stop without retry"}
  $summary=Get-Content -LiteralPath "$capture/summary.json" -Raw | ConvertFrom-Json
  if(-not $summary.completed -or -not $summary.diagnostics_stable -or $summary.target_pid -ne $engine.Id -or $summary.target_creation_time -ne $engineCreated -or -not $summary.output_exported){throw 'Stable complete native output required'}
  if($engine.HasExited){throw 'Audited audio engine exited during test'}
  $completed=$true
 }
}catch{$issue=$_.Exception.Message;$issue | Set-Content -LiteralPath "$out/error.txt" -Encoding utf8}
finally{Save-Json 'exit.json' ([ordered]@{workflow_completed=$completed;mode=$Mode;error=$issue;native_launcher_started=$launched;probe_pid=$(if($child){$child.Id}else{$null})
 probe_exited=$(if($child){$child.HasExited}else{$true});probe_exit=$(if($child -and $child.HasExited){$child.ExitCode}else{$null});mat_verified=$false;automatic_retry=$false;forced_termination=$false})}
if(-not $completed){exit 1}
