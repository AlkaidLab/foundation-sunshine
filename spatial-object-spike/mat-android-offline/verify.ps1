param(
 [Parameter(Mandatory=$true)][string]$Output,
 [string]$SdkRoot=$env:ANDROID_HOME,
 [string]$JdkRoot=$env:JAVA_HOME,
 [string]$Python='python.exe',
 [string]$SigningKeystore
)
$ErrorActionPreference='Stop'
if (!$JdkRoot -or !(Test-Path -LiteralPath "$JdkRoot/include/jni.h")) { throw 'Set -JdkRoot or JAVA_HOME to a JDK with JNI headers' }
if (!$SdkRoot -or !(Test-Path -LiteralPath "$SdkRoot/platforms/android-36/android.jar")) { throw 'Set -SdkRoot or ANDROID_HOME to an Android SDK with platform 36' }
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'A new output directory is required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$env:JAVA_HOME=$JdkRoot
$env:PATH="$JdkRoot/bin;C:/msys64/ucrt64/bin;"+$env:PATH
$app="$root/spatial-object-spike/moonlight-vplus/app"
$jni="$app/src/main/jni/moonlight-core"
$common="$jni/moonlight-common-c"
$source="$root/spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw"
$expected='79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6'
if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLower() -ne $expected){throw 'Frozen MAT fixture changed'}
$audio="$app/src/main/java/com/limelight/binding/audio"
New-Item -ItemType Directory -Path "$out/classes","$out/dex" | Out-Null
& gcc.exe -shared -O2 -std=c11 -Wall -Wextra -Werror -static-libgcc "-I$JdkRoot/include" "-I$JdkRoot/include/win32" "$jni/mat_carrier_bridge.c" "$common/src/MatReceiver.c" "$common/src/MatDepacketizer.c" -o "$out/moonlight-core.dll"
if($LASTEXITCODE){throw 'Host JNI build failed'}
$core=@('MatCarrierConsumer','MatCarrierPlayer','MatNativeReceiver','MatRecording','MatOutputPolicy')|ForEach-Object {"$audio/$_.java"}
& javac.exe --release 8 -encoding UTF-8 -d "$out/classes" @core "$app/src/test/java/com/limelight/binding/audio/MatOfflineTest.java" "$PSScriptRoot/MatResearchPcm.java" "$PSScriptRoot/MatResearchPcmTest.java"
if($LASTEXITCODE){throw 'Core Java compile failed'}
& java.exe -Xcheck:jni "-Djava.library.path=$out" -cp "$out/classes" com.limelight.binding.audio.MatOfflineTest $source 2>&1 | Tee-Object -FilePath "$out/jvm-tests.log"
if($LASTEXITCODE){throw 'Production JNI/player tests failed'}
& javac.exe --release 8 -encoding UTF-8 -classpath "$out/classes" -d "$out/classes" "$app/src/test/java/com/limelight/binding/audio/MatOutputPolicyTest.java"
if($LASTEXITCODE){throw 'MAT output policy tests compile failed'}
& java.exe -cp "$out/classes" com.limelight.binding.audio.MatOutputPolicyTest | Tee-Object -FilePath "$out/output-policy.log"
if($LASTEXITCODE){throw 'MAT output policy regression failed'}
& $Python "$PSScriptRoot/phone_pcm_oracle.py" $source "$out/research-mono.pcm" | Tee-Object -FilePath "$out/phone-pcm-oracle.log"
if($LASTEXITCODE){throw 'Python research PCM oracle failed'}
& java.exe -cp "$out/classes" com.limelight.binding.audio.MatResearchPcmTest $source "$out/research-mono.pcm" | Tee-Object -FilePath "$out/phone-pcm-test.log"
if($LASTEXITCODE){throw 'Research PCM does not match existing offline extraction'}
$androidJar="$SdkRoot/platforms/android-36/android.jar"
& javac.exe -source 8 -target 8 -encoding UTF-8 -classpath "$androidJar;$out/classes" -d "$out/classes" "$audio/MatOutputCapabilities.java" "$audio/MatAudioTrackSink.java" "$app/src/debug/java/com/limelight/binding/audio/MatReplayActivity.java" "$PSScriptRoot/MatDeviceCheck.java" "$PSScriptRoot/MatResearchSpeaker.java"
if($LASTEXITCODE){throw 'Actual Android AudioTrack sink/activity compile failed'}
# Build both common-c copies and rerun the pre-existing dump/manifest acceptance tests.
foreach($variant in @('host-common','android-common')) {
 $dir=if($variant -eq 'host-common'){"$root/third-party/moonlight-common-c"}else{$common}
 & cmake.exe -S $dir -B "$out/$variant" -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64
 if($LASTEXITCODE){throw "$variant configure failed"}
 & cmake.exe --build "$out/$variant" --parallel 4
 if($LASTEXITCODE){throw "$variant build failed"}
 & ctest.exe --test-dir "$out/$variant" --output-on-failure
 if($LASTEXITCODE){throw "$variant common regression tests failed"}
}
& "$root/spatial-object-spike/mat-rtsp-spike/build-tests.ps1" -Build $out
if($LASTEXITCODE){throw 'Dump regression build failed'}
& "$out/mat-profile-unit.exe" $source "$out/dump-regression" 2>&1 | Tee-Object -FilePath "$out/dump-regression.log"
if($LASTEXITCODE){throw 'Original dump/manifest lifecycle regression failed'}
& "$root/spatial-object-spike/mat-rtsp-spike/check-android.ps1" -Output "$out/android-objects" -SdkRoot $SdkRoot *>&1 | Tee-Object -FilePath "$out/android-check.log"
if($LASTEXITCODE){throw 'Actual Android common/JNI compile failed'}
# Standalone diagnostic APK, using the exact same client classes and JNI source.
# This is NOT a full Moonlight APK and has no network or storage permissions.
$clang="$SdkRoot/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
foreach($abi in @('arm64-v8a','armeabi-v7a')) {
 $target=if($abi -eq 'arm64-v8a'){'aarch64-linux-android24'}else{'armv7a-linux-androideabi24'}
 New-Item -ItemType Directory -Path "$out/apk/lib/$abi" -Force | Out-Null
 & $clang "--target=$target" -shared -fPIC -O2 -std=c11 -Wall -Wextra -Werror '-Wl,--no-undefined' "$jni/mat_carrier_bridge.c" "$common/src/MatReceiver.c" "$common/src/MatDepacketizer.c" -o "$out/apk/lib/$abi/libmoonlight-core.so"
 if($LASTEXITCODE){throw "$abi MAT JNI module build failed"}
}
$tools="$SdkRoot/build-tools/36.0.0"
$classFiles=@((Get-ChildItem -LiteralPath "$out/classes" -Recurse -Filter '*.class').FullName)
& "$tools/d8.bat" --lib $androidJar --min-api 24 --output "$out/dex" @classFiles
if($LASTEXITCODE){throw 'Diagnostic DEX compile failed'}
& "$tools/aapt2.exe" link -o "$out/unsigned.apk" -I $androidJar --manifest "$PSScriptRoot/AndroidManifest.xml" --min-sdk-version 24 --target-sdk-version 35 --debug-mode
if($LASTEXITCODE){throw 'Diagnostic manifest link failed'}
New-Item -ItemType Directory -Path "$out/apk/assets" | Out-Null
Copy-Item -LiteralPath $source -Destination "$out/apk/assets/verified-mat.raw"
& jar.exe uf "$out/unsigned.apk" -C "$out/dex" classes.dex -C "$out/apk" lib -C "$out/apk" assets
if($LASTEXITCODE){throw 'Diagnostic APK package failed'}
& "$tools/zipalign.exe" -f -P 16 4 "$out/unsigned.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Diagnostic APK alignment failed'}
if($SigningKeystore){
 Copy-Item -LiteralPath $SigningKeystore -Destination "$out/mat-debug.keystore"
} else {
 & keytool.exe -genkeypair -keystore "$out/mat-debug.keystore" -storepass android -keypass android -alias mat -dname 'CN=Local MAT Offline Diagnostic' -keyalg RSA -keysize 2048 -validity 3650 -noprompt
 if($LASTEXITCODE){throw 'Local diagnostic debug signing key failed'}
}
& "$tools/apksigner.bat" sign --ks "$out/mat-debug.keystore" --ks-key-alias mat --ks-pass pass:android --key-pass pass:android --out "$out/mat-offline-diagnostic.apk" "$out/aligned.apk"
if($LASTEXITCODE){throw 'Diagnostic APK signing failed'}
& "$tools/apksigner.bat" verify --verbose "$out/mat-offline-diagnostic.apk"
if($LASTEXITCODE){throw 'Diagnostic APK signature failed'}
& "$tools/aapt2.exe" dump badging "$out/mat-offline-diagnostic.apk" | Tee-Object -FilePath "$out/apk-badging.log"
if($LASTEXITCODE){throw 'Diagnostic APK inspection failed'}
if((Get-Content -Raw "$out/apk-badging.log") -match 'uses-permission:'){throw 'Offline diagnostic unexpectedly requests permissions'}
foreach($name in @('MatReceiver.c','MatReceiver.h','MatDumpFile.c','MatDepacketizer.c','MatDepacketizer.h','MatWire.h','MatDumpProfile.h')) {
 $hostText=(Get-Content -Raw "$root/third-party/moonlight-common-c/src/$name").Replace("`r`n","`n")
 $androidText=(Get-Content -Raw "$common/src/$name").Replace("`r`n","`n")
 if($hostText -cne $androidText){throw "Host/Android common source mismatch: $name"}
}
$files=@($core)+@("$audio/MatAudioTrackSink.java","$jni/mat_carrier_bridge.c","$jni/Android.mk","$app/proguard-rules.pro","$app/src/debug/java/com/limelight/binding/audio/MatReplayActivity.java","$app/src/debug/AndroidManifest.xml","$app/src/test/java/com/limelight/binding/audio/MatOfflineTest.java","$common/src/MatReceiver.h","$common/src/MatReceiver.c","$common/src/MatDepacketizer.c","$common/src/MatDumpFile.c","$PSScriptRoot/verify.ps1","$PSScriptRoot/AndroidManifest.xml","$PSScriptRoot/MatDeviceCheck.java","$out/mat-offline-diagnostic.apk")
$files+=@("$audio/MatOutputCapabilities.java","$app/src/test/java/com/limelight/binding/audio/MatOutputPolicyTest.java")
$files+=@('MatResearchPcm.java','MatResearchPcmTest.java','MatResearchSpeaker.java','phone_pcm_oracle.py') | ForEach-Object {"$PSScriptRoot/$_"}
$files+="$root/spatial-object-spike/mat-spike/analyze_mat.py"
$hashes=@($files|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLower()}})
@{result='PASS';scope='A/B offline: real JNI and fake hardware; Android sources/DEX/native modules/APK built, no device playback';cycles=178;bytes=10936320;sha256=$expected;files=$hashes}|ConvertTo-Json -Depth 5 | Out-File -LiteralPath "$out/proof.json" -Encoding utf8
Write-Host "PASS A/B offline: $out/proof.json"
