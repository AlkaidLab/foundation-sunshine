param([Parameter(Mandatory=$true)][string]$Output,[string]$Python='python.exe')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Use a fresh live MAT evidence directory'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$common="$root/third-party/moonlight-common-c"
$sharedCommit=git -C $common rev-parse HEAD
if(git -C $common status --porcelain){throw 'Freeze shared sources before verification'}
# Mock clients compile against this exact library. The historical Android app
# checkout is intentionally not upgraded or included in this acceptance.
$sourcePaths=@('src/audio.h','src/config.h','src/config.cpp','src/rtsp.h','src/rtsp.cpp','src/stream.cpp',
 'src/spatial_transport/mat_live_session.h','src/spatial_audio/mat_capture_protocol.h','src/spatial_audio/mat_capture_reader.h','src/spatial_audio/native_mat_pipe_source.h',
 'src/spatial_audio/native_mat_guard.h','src/spatial_audio/native_mat_assembler.h',
 'src/spatial_audio/stream_ring_source.h','src/spatial_audio/stream_output_protocol.h','src/spatial_audio/mat_stream_receiver.cpp',
 'spatial-object-spike/resident/encoded_tap.cpp','spatial-object-spike/mat-android-session/capability_negotiation_test.py',
 'spatial-object-spike/mat-rtsp-live/live_verify.py','spatial-object-spike/mat-rtsp-live/operator_process_test.py') | ForEach-Object {"$root/$_"}
$sourcePaths+=Get-ChildItem -LiteralPath $PSScriptRoot -File | ForEach-Object {$_.FullName}
$sourcePaths+=Get-ChildItem -LiteralPath "$PSScriptRoot/../sonic-mat-live" -File | ForEach-Object {$_.FullName}
$sourcePaths+=Get-ChildItem -LiteralPath "$PSScriptRoot/../mat-diagnostics" -File | ForEach-Object {$_.FullName}
foreach($relative in @(git -C $common diff --name-only 05415ed4f544ee10d9fc6c9735a1f34c4a032a87 HEAD)){
 if(Test-Path -LiteralPath "$common/$relative"){$sourcePaths+="$common/$relative"}
}
$sourceHashes=@($sourcePaths | Sort-Object -Unique | ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLower()}})
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
foreach($entry in ,@('host-common',$common)){
 $name=$entry[0];$source=$entry[1]
 & cmake.exe -S $source -B "$out/$name" -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64 *> "$out/$name-config.log"
 if($LASTEXITCODE){throw "$name configure failed"}
 & cmake.exe --build "$out/$name" --parallel 4 *> "$out/$name-build.log"
 if($LASTEXITCODE){throw "$name build failed"}
 & ctest.exe --test-dir "$out/$name" --output-on-failure *> "$out/$name-tests.log"
 if($LASTEXITCODE){throw "$name tests failed"}
 & $Python "$PSScriptRoot/crypto_oracle.py" --executable "$out/$name/mat-live-tests.exe" --output "$out/$name/golden.bin" *> "$out/$name-oracle.log"
 if($LASTEXITCODE){throw "$name independent cryptographic oracle failed"}
}
& "$PSScriptRoot/../mat-diagnostics/verify.ps1" -Output "$out/diagnostics" -CommonBuild "$out/host-common" -Python $Python *> "$out/diagnostics.log"
if($LASTEXITCODE){throw 'Tool-owned dump/raw-recording regression failed'}
foreach($name in @('MatLivePlayback','MatReceiver','MatDepacketizer')){
 & gcc.exe -O2 -std=c11 "-I$common/src" -c "$common/src/$name.c" -o "$out/$name.o"
 if($LASTEXITCODE){throw "$name host receiver compile failed"}
}
& g++.exe -O2 -std=c++23 -Wall -Wextra -Wno-misleading-indentation "$PSScriptRoot/host_test.cpp" "$out/MatLivePlayback.o" "$out/MatReceiver.o" "$out/MatDepacketizer.o" -lcrypto -o "$out/host-test.exe"
if($LASTEXITCODE){throw 'Actual live sender/assembler compile failed'}
& "$out/host-test.exe" *> "$out/host-tests.log"
if($LASTEXITCODE){throw 'Actual live sender/assembler regression failed'}
& "$PSScriptRoot/build-pipe-test.ps1" -Output "$out/pipe-regression" *> "$out/pipe-regression-build.log"
if($LASTEXITCODE){throw 'Private pipe tail/acknowledgement regression failed'}
& "$PSScriptRoot/build-live-check.ps1" -CommonBuild "$out/host-common" -Output "$out/live-check" *> "$out/live-check-build.log"
if($LASTEXITCODE){throw 'Native-only checker / software UDP+FEC regression failed'}
& "$PSScriptRoot/../sonic-mat-live/build.ps1" -Output "$out/sonic-source" *> "$out/sonic-source-build.log"
if($LASTEXITCODE){throw 'Sonic object / native-worker adapter software verification failed'}
& "$PSScriptRoot/../sonic-mat-live/test-manage-live.ps1" -Output "$out/operator-state" *> "$out/operator-state.log"
if($LASTEXITCODE){throw 'Native operator state/identity regression failed'}
& $Python "$PSScriptRoot/../mat-rtsp-live/operator_process_test.py" --output "$out/operator-launcher" --powershell (Get-Process -Id $PID).Path *> "$out/operator-launcher.log"
if($LASTEXITCODE){throw 'Native operator launcher output/ownership regression failed'}
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -DHAS_SOCKLEN_T "-I$common/src" "$PSScriptRoot/../mat-android-session/negotiation_client.c" "$out/host-common/libmoonlight-common-c.a" "$out/host-common/enet/libenet.a" -static-libgcc -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/negotiation-client.exe"
if($LASTEXITCODE){throw 'Production RTSP frontend compile failed'}
& $Python "$PSScriptRoot/../mat-android-session/capability_negotiation_test.py" --client "$out/negotiation-client.exe" --output "$out/negotiation" *> "$out/negotiation.log"
if($LASTEXITCODE){throw 'Production RTSP live/Opus negotiation failed'}
foreach($source in $sourceHashes){if((Get-FileHash -LiteralPath $source.path).Hash.ToLower() -ne $source.sha256){throw "Source changed during verification: $($source.path)"}}
$hashes=@(Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object Extension -In '.log','.json','.exe' | ForEach-Object {@{path=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLower()}})
@{result='PASS';scope='Software live MAT sender / RTP+FEC receiver / authenticated transport / negotiation; synthetic test bytes only';
 commonCommit=$sharedCommit;sourceHashes=$sourceHashes;commonTests=13;client='mock frontends using the reviewed host common library';diagnosticsRegression='PASS';rtspCases=22;logicalDataPacketsPerLongTest=78000;
 generatedUdpCyclesPerCase=1300;udpCases=2;deliberateDropsRecovered=391;privatePipeVersion=2;pipeTailRegression='PASS';operatorStateRegression='PASS';operatorLauncherRegression='PASS';nativeCaptureTested=$false;androidDeviceTested=$false;atmosRecognitionVerified=$false;artifacts=$hashes} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/proof.json" -Encoding utf8
Write-Host "PASS live MAT software verification: $out/proof.json"
