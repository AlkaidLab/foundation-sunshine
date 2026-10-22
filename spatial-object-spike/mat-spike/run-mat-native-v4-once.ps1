param(
 [string]$Build="$PSScriptRoot/mat-native-v4-build01",
 [string]$InputDirectory="$PSScriptRoot/mat-native-v4-input01",
 [string]$InputProof="$PSScriptRoot/mat-native-v4-input-proof01",
 [string]$Output="$PSScriptRoot/mat-native-v4-run01"
)
# Prepared only. Run once solely after new explicit authorization; never self-elevate.
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Separately approved one-shot elevated launcher required'}
if(Test-Path -LiteralPath $Output){throw 'NEW output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
$child=$null;$engine=$null;$failed=$null;$completed=$false
function Save-Json($Name,$Value){$Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out $Name) -Encoding utf8}
function Check-Hash($Path,$Hash){if((Get-FileHash -LiteralPath $Path).Hash -ne $Hash){throw "Hash mismatch: $Path"}}
try{
 $manifest=Get-Content -LiteralPath "$Build/build.json" -Raw | ConvertFrom-Json
 foreach($source in $manifest.source_files){Check-Hash "$PSScriptRoot/$($source.file)" $source.sha256}
 $exe=(Resolve-Path -LiteralPath "$Build/mat-replay-v4-probe.exe").Path
 $helper=(Resolve-Path -LiteralPath "$Build/mat_replay_v4.dll").Path
 Check-Hash $exe $manifest.probe_sha256;Check-Hash $helper $manifest.dll_sha256
 $inputFile=(Resolve-Path -LiteralPath "$InputDirectory/input.bin").Path
 $inputManifest=Get-Content -LiteralPath "$InputDirectory/manifest.json" -Raw | ConvertFrom-Json
 $inputChecked=Get-Content -LiteralPath "$InputProof/proof.json" -Raw | ConvertFrom-Json
 if(-not $inputChecked.passed -or $inputManifest.scheduled_seconds -ne 6.12 -or $inputManifest.source_audio_samples -ne 384000 -or $inputChecked.counts.unity_submissions -ne 863){throw 'Expected verified bounded two-domain fixture'}
 Check-Hash $inputFile $manifest.input_sha256;Check-Hash $inputFile $inputManifest.input_sha256;Check-Hash $inputFile $inputChecked.input_sha256
 Check-Hash "$InputDirectory/manifest.json" $inputChecked.input_manifest_sha256
 Check-Hash $exe $inputChecked.probe_sha256
 foreach($entry in $inputChecked.expected_files){Check-Hash "$InputProof/$($entry.file)" $entry.sha256}
 Check-Hash 'C:/Windows/System32/audioeng.dll' '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3'
 if(@(Get-Process collector,quantum-collector,quantum-v4-collector,mat-lifecycle-probe,mat-encode-probe,mat-replay-v4-probe -ErrorAction SilentlyContinue).Count){throw 'Another audio probe is running'}
 # Read-only capability query may start an idle engine. No playback/endpoint switch.
 $auditExe=(Resolve-Path -LiteralPath "$PSScriptRoot/../spatial_probe.exe").Path
 Check-Hash $auditExe 'DB7A536ACBBD82180FBE52D036B12B279EE0F13C60C5014535ED5E2D21FD4EAC'
 $audit=& $auditExe --list;$auditExit=$LASTEXITCODE
 $audit | Set-Content -LiteralPath "$out/host-audit.txt" -Encoding utf8
 if($auditExit){throw 'Read-only host capability query failed'}
 $watch=[Diagnostics.Stopwatch]::StartNew();$engines=@(Get-Process audiodg -ErrorAction SilentlyContinue)
 while($engines.Count -eq 0 -and $watch.Elapsed.TotalSeconds -lt 3){Start-Sleep -Milliseconds 100;$engines=@(Get-Process audiodg -ErrorAction SilentlyContinue)}
 if($engines.Count -ne 1){throw 'Exactly one current audiodg required; no automatic restart or playback'}
 $engine=$engines[0];$heldEngine=$engine.Handle
 $packages=@(Get-AppxPackage -Name DolbyLaboratories.DolbyAccess)
 if($packages.Count -ne 1){throw 'Expected one installed Dolby Access package'}
 $vendor=Join-Path $packages[0].InstallLocation 'DLLs/x64'
 $vendorHashes=@{'DolbyMatEnc.dll'='963697FB61EE302699B294B1B6CB59AB42E5EAE3A555F6674ED54FC3A3CFACEA';'DolbyMatEngine.dll'='342CBC58A19AFA2B5B58CEF408EF91DCC06C68829520286ACABB7178EDA7F569'}
 foreach($name in $vendorHashes.Keys){Check-Hash (Join-Path $vendor $name) $vendorHashes[$name]}
 $validation=& $exe --validate $inputFile (Join-Path $out 'input-validation');$validationExit=$LASTEXITCODE
 $validation | Set-Content -LiteralPath "$out/input-validation.stdout.txt" -Encoding utf8
 if($validationExit){throw 'Input preflight rejected; stop before remote code'}
 $deployment='C:/ProgramData/SpatialObjectSpike'
 if(-not(Test-Path -LiteralPath $deployment)){throw 'Expected existing helper directory'}
 $deployed=Join-Path $deployment 'mat_replay_v4.dll'
 if(Test-Path -LiteralPath $deployed){Check-Hash $deployed $manifest.dll_sha256}else{Copy-Item -LiteralPath $helper -Destination $deployed;Check-Hash $deployed $manifest.dll_sha256}
 Save-Json 'launch.json' ([ordered]@{approved_scope='One UAC, one two-domain native MAT replay; 6.12 seconds scheduled input';started=(Get-Date).ToString('o')
  audiodg_pid_before=$engine.Id;helper=$deployed;helper_sha256=$manifest.dll_sha256;probe_sha256=$manifest.probe_sha256;input_sha256=$inputManifest.input_sha256
  input_manifest_sha256=(Get-FileHash -LiteralPath "$InputDirectory/manifest.json").Hash;input_proof_sha256=(Get-FileHash -LiteralPath "$InputProof/proof.json").Hash
  launcher_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;vendor_directory=$vendor;vendor_hashes=$vendorHashes
  source_process_blocks=512;preroll_blocks=40;drain_blocks=60;scheduled_process_blocks=612;volume=1;call_gain=1;worker_watchdog_seconds=10
  endpoint_changes=$false;driver_changes=$false;security_changes=$false;hooks=$false;game_started=$false;automatic_retry=$false;mat_verified=$false
  warning='Private version-locked ABI on own audiodg worker. No forced thread/engine termination; pinned helper/vendor refs remain until natural engine exit. No decoder/Android claim.'})
 $capture=Join-Path $out 'capture'
 $child=Start-Process -FilePath $exe -ArgumentList @('--capture',"`"$capture`"","`"$deployed`"","`"$vendor`"","`"$inputFile`"") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/probe.stdout.txt" -RedirectStandardError "$out/probe.stderr.txt"
 $heldChild=$child.Handle
 if(-not $child.WaitForExit(20000)){throw 'Probe did not exit within bound; do not forcibly terminate or retry'}
 $child.Refresh()
 if($child.ExitCode -ne 0){throw "Probe exited $($child.ExitCode); no retry"}
 $summary=Get-Content -LiteralPath "$capture/summary.json" -Raw | ConvertFrom-Json
 if(-not $summary.thread_completed -or $summary.status -ne 2 -or $summary.error -ne 0 -or -not $summary.guard_passed -or $summary.engine_pid -ne $engine.Id){throw 'Native completion/host guard failed'}
 foreach($domain in 1,2){$report=Get-Content -LiteralPath "$capture/domain-$domain/summary.json" -Raw | ConvertFrom-Json;if($report.status -ne 2 -or $report.error -ne 0 -or $report.processed_blocks -ne $report.scheduled_blocks){throw "Incomplete domain $domain"}}
 if($engine.HasExited){throw 'Audited engine exited during test'}
 $completed=$true
}catch{$failed=$_.Exception.Message;$failed | Set-Content -LiteralPath "$out/launcher-error.txt" -Encoding utf8}finally{
 Save-Json 'exit.json' ([ordered]@{completed=$completed;error=$failed;probe_pid=$(if($child){$child.Id}else{$null});probe_exited=$(if($child){$child.HasExited}else{$true})
  probe_exit=$(if($child -and $child.HasExited){$child.ExitCode}else{$null});audited_engine_exited=$(if($engine){$engine.HasExited}else{$null})
  audiodg_pids_after=@((Get-Process audiodg -ErrorAction SilentlyContinue).Id);finished=(Get-Date).ToString('o');mat_verified=$false;automatic_retry=$false})
}
if(-not $completed){exit 1}
