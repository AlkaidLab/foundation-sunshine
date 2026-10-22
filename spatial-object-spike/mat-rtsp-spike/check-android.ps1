param([Parameter(Mandatory=$true)][string]$Output,
 [string]$SdkRoot=$env:ANDROID_HOME,
 [string]$AudioHapticsSdkDir="$PSScriptRoot/../../third-party/moonlight-audio-haptics")
$ErrorActionPreference='Stop';$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'New Android object output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$jni="$root/spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core";$common="$jni/moonlight-common-c"
$ndk="$SdkRoot/ndk/28.2.13676358"
$compiler="$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
if (!(Test-Path -LiteralPath $compiler)) { throw 'Set -SdkRoot or ANDROID_HOME to an SDK with NDK 28.2.13676358' }
$flags=@('--target=aarch64-linux-android22','-fPIC','-O2','-std=c11','-DLC_ANDROID','-DHAS_SOCKLEN_T=1','-DHAVE_CLOCK_GETTIME=1',"-I$common/src","-I$common/enet/include","-I$common/nanors","-I$common/nanors/deps/obl","-I$common/nanors/deps","-I$jni/libopus/include","-I$jni/openssl/include","-I$ndk/sources/android/cpufeatures","-I$AudioHapticsSdkDir/platform/android/src/main/cpp/include")
foreach($file in @('AudioStream','MatDepacketizer','MatReceiver','MatDumpFile','MatPlayback','MatLivePlayback','Connection','SdpGenerator','RtspConnection','RtspParser')) {
 & $compiler @flags -c "$common/src/$file.c" -o "$out/$file.o"
 if($LASTEXITCODE){throw "Android arm64 compile $file failed"}
}
foreach($file in @('callbacks','simplejni','mat_carrier_bridge')) {
 & $compiler @flags -c "$jni/$file.c" -o "$out/$file.o"
 if($LASTEXITCODE){throw "Android JNI compile $file failed"}
}
& $compiler --target=aarch64-linux-android22 -shared '-Wl,--no-undefined' "$out/MatDepacketizer.o" "$out/MatReceiver.o" "$out/MatDumpFile.o" "$out/MatPlayback.o" "$out/MatLivePlayback.o" "$out/mat_carrier_bridge.o" "$jni/openssl/arm64-v8a/libcrypto.a" -ldl -llog -o "$out/libmat-dump-check.so"
if($LASTEXITCODE){throw 'Android MAT dump shared-module link failed'}
Write-Host 'PASS actual Android arm64 common/JNI objects and MAT dump .so link; NOT full APK link/device test'
