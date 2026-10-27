param([Parameter(Mandatory=$true)][string]$Output,[string]$Python='python.exe')
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Use a fresh live MAT evidence directory'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$common="$root/third-party/moonlight-common-c"
$sharedCommit=git -C $common rev-parse HEAD
if(git -C $common status --porcelain){throw 'Freeze shared sources before verification'}
# Extended software integration only; routine MAT checks are registered in CTest.
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
& "$PSScriptRoot/verify-udp.ps1" -Output "$out/udp-watchdog" -CommonBuild "$out/host-common" -Python $Python *> "$out/udp-watchdog.log"
if($LASTEXITCODE){throw 'Live-only UDP watchdog regression failed'}
foreach($name in @('MatLivePlayback','MatReceiver','MatDepacketizer')){
 & gcc.exe -O2 -std=c11 "-I$common/src" -c "$common/src/$name.c" -o "$out/$name.o"
 if($LASTEXITCODE){throw "$name host receiver compile failed"}
}
& g++.exe -O2 -std=c++23 -Wall -Wextra -Wno-misleading-indentation "$PSScriptRoot/host_test.cpp" "$out/MatLivePlayback.o" "$out/MatReceiver.o" "$out/MatDepacketizer.o" -lcrypto -lbcrypt -ladvapi32 -lshell32 -lole32 -luuid -o "$out/host-test.exe"
if($LASTEXITCODE){throw 'Actual live sender/assembler compile failed'}
& "$out/host-test.exe" *> "$out/host-tests.log"
if($LASTEXITCODE){throw 'Actual live sender/assembler regression failed'}
& "$root/tools/mat-live/build-live-check.ps1" -CommonBuild "$out/host-common" -Output "$out/live-check" *> "$out/live-check-build.log"
if($LASTEXITCODE){throw 'Native-only checker / software UDP+FEC regression failed'}
& "$root/tools/sonic-mat-live/build.ps1" -Output "$out/plugin-source" *> "$out/plugin-source-build.log"
if($LASTEXITCODE){throw 'Plugin object / native-worker adapter software verification failed'}
& "$root/tools/sonic-mat-live/test-manage-live.ps1" -Output "$out/operator-state" *> "$out/operator-state.log"
if($LASTEXITCODE){throw 'Native operator state/identity regression failed'}
& $Python "$PSScriptRoot/operator_process_test.py" --output "$out/operator-launcher" --powershell (Get-Process -Id $PID).Path *> "$out/operator-launcher.log"
if($LASTEXITCODE){throw 'Native operator launcher output/ownership regression failed'}
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -DHAS_SOCKLEN_T "-I$common/src" "$PSScriptRoot/negotiation_client.c" "$out/host-common/libmoonlight-common-c.a" "$out/host-common/enet/libenet.a" -static-libgcc -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/negotiation-client.exe"
if($LASTEXITCODE){throw 'Production RTSP frontend compile failed'}
& $Python "$PSScriptRoot/negotiation_test.py" --client "$out/negotiation-client.exe" --output "$out/negotiation" *> "$out/negotiation.log"
if($LASTEXITCODE){throw 'Production RTSP live/Opus negotiation failed'}
@{result='PASS';scope='Extended software integration; generated data and mock clients';commonCommit=$sharedCommit;
 nativeCaptureTested=$false;androidDeviceTested=$false;atmosRecognitionVerified=$false} | ConvertTo-Json | Set-Content -LiteralPath "$out/proof.json" -Encoding utf8
Write-Host "PASS live MAT software verification: $out/proof.json"
