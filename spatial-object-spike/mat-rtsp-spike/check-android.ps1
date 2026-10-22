param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop';$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'New Android object output required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$jni="$root/spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core";$common="$jni/moonlight-common-c"
$compiler='C:/Users/chou0/AppData/Local/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
$flags=@('--target=aarch64-linux-android22','-fPIC','-O2','-std=c11','-DLC_ANDROID','-DHAS_SOCKLEN_T=1','-DHAVE_CLOCK_GETTIME=1',"-I$common/src","-I$common/enet/include","-I$common/nanors","-I$common/nanors/deps/obl","-I$common/nanors/deps","-I$jni/libopus/include","-I$jni/openssl/include","-IC:/Users/chou0/AppData/Local/Android/Sdk/ndk/28.2.13676358/sources/android/cpufeatures","-I$root/third-party/moonlight-audio-haptics/platform/android/src/main/cpp/include")
foreach($file in @('AudioStream','MatDepacketizer','MatReceiver','MatDumpFile','MatPlayback','Connection','SdpGenerator','RtspConnection')) {
 & $compiler @flags -c "$common/src/$file.c" -o "$out/$file.o"
 if($LASTEXITCODE){throw "Android arm64 compile $file failed"}
}
foreach($file in @('callbacks','simplejni','mat_carrier_bridge')) {
 & $compiler @flags -c "$jni/$file.c" -o "$out/$file.o"
 if($LASTEXITCODE){throw "Android JNI compile $file failed"}
}
& $compiler --target=aarch64-linux-android22 -shared '-Wl,--no-undefined' "$out/MatDepacketizer.o" "$out/MatReceiver.o" "$out/MatDumpFile.o" "$out/MatPlayback.o" "$out/mat_carrier_bridge.o" "$jni/openssl/arm64-v8a/libcrypto.a" -ldl -llog -o "$out/libmat-dump-check.so"
if($LASTEXITCODE){throw 'Android MAT dump shared-module link failed'}
Write-Host 'PASS actual Android arm64 common/JNI objects and MAT dump .so link; NOT full APK link/device test'
