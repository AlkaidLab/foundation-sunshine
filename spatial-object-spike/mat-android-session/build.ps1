param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'New build directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$env:JAVA_HOME='C:/Program Files/Java/jdk-22';$env:PATH="$env:JAVA_HOME/bin;C:/msys64/ucrt64/bin;"+$env:PATH
$jni="$root/spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core";$common="$jni/moonlight-common-c"
& cmake.exe -S $common -B "$out/common" -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64
if($LASTEXITCODE){throw 'common configure failed'}
& cmake.exe --build "$out/common" --parallel 4
if($LASTEXITCODE){throw 'common build failed'}
& gcc.exe -shared -std=c11 -O2 -DHAS_SOCKLEN_T "-I$env:JAVA_HOME/include" "-I$env:JAVA_HOME/include/win32" "-I$jni" "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/session_jni.c" "$jni/mat_carrier_bridge.c" "$out/common/libmoonlight-common-c.a" "$out/common/enet/libenet.a" -static-libgcc -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/moonlight-mat-session.dll"
if($LASTEXITCODE){throw 'Session JNI DLL build failed'}
New-Item -ItemType Directory -Path "$out/classes" | Out-Null
$audio="$root/spatial-object-spike/moonlight-vplus/app/src/main/java/com/limelight/binding/audio"
$files=@('MatCarrierConsumer','MatCarrierPlayer','MatPlaybackSession','MatRecording')|ForEach-Object {"$audio/$_.java"}
& javac.exe --release 8 -encoding UTF-8 -d "$out/classes" @files "$PSScriptRoot/MatSessionClient.java"
if($LASTEXITCODE){throw 'Production Java session compile failed'}
@("$env:JAVA_HOME/bin/java.exe",'-Xcheck:jni',"-Djava.library.path=$out",'-cp',"$out/classes",'com.limelight.binding.audio.MatSessionClient')|ConvertTo-Json|Out-File -LiteralPath "$out/client-command.json" -Encoding utf8
Write-Host "READY $out/client-command.json"
