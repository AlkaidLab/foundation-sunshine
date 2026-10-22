param(
 [Parameter(Mandatory=$true)][string]$Output,
 [string]$SdkRoot=$env:ANDROID_HOME,[string]$JdkRoot=$env:JAVA_HOME,[string]$Python='python.exe',
 [switch]$BuildFullClient,[string]$AndroidBuildRoot
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh capability evidence directory required'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$client="$root/spatial-object-spike/moonlight-vplus"
$common="$root/third-party/moonlight-common-c"
$clientCommon="$client/app/src/main/jni/moonlight-core/moonlight-common-c"
$env:JAVA_HOME=$JdkRoot;$env:ANDROID_HOME=$SdkRoot
$env:PATH="$JdkRoot/bin;C:/msys64/ucrt64/bin;"+$env:PATH
$sharedHead=git -C $common rev-parse HEAD
if($sharedHead -ne (git -C $clientCommon rev-parse HEAD)){throw 'Both parents must use the same common-c commit'}
foreach($path in @($common,$clientCommon)){
 if(git -C $path status --porcelain){throw 'Commit shared sources before freezing verification'}
}
& "$root/spatial-object-spike/mat-android-offline/verify.ps1" -Output "$out/offline" -SdkRoot $SdkRoot -JdkRoot $JdkRoot -Python $Python *> "$out/offline.log"
if($LASTEXITCODE){throw 'Offline/JNI/policy/common/Android regression failed'}
& gcc.exe -O2 -std=c11 -Wall -Wextra -Werror -DHAS_SOCKLEN_T "-I$common/src" "$root/spatial-object-spike/mat-android-session/negotiation_client.c" "$out/offline/host-common/libmoonlight-common-c.a" "$out/offline/host-common/enet/libenet.a" -static-libgcc -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/negotiation-client.exe"
if($LASTEXITCODE){throw 'Actual RTSP negotiation frontend compile failed'}
& $Python "$root/spatial-object-spike/mat-android-session/capability_negotiation_test.py" --client "$out/negotiation-client.exe" --output "$out/negotiation" *> "$out/negotiation.log"
if($LASTEXITCODE){throw 'Automatic negotiation regression failed'}
& "$root/spatial-object-spike/mat-android-session/build.ps1" -Output "$out/strict-client" -JdkRoot $JdkRoot *> "$out/strict-build.log"
if($LASTEXITCODE){throw 'Strict JNI session build failed'}
& $Python "$root/spatial-object-spike/mat-android-session/negotiation_test.py" --client "$out/strict-client/client-command.json" --output "$out/strict-negative" *> "$out/strict-negative.log"
if($LASTEXITCODE){throw 'Strict MAT diagnostic negative regression failed'}
$apk=$null
if($BuildFullClient){
 if(!$AndroidBuildRoot -or $AndroidBuildRoot.Contains("'") -or $AndroidBuildRoot.Contains("`n") -or $AndroidBuildRoot.Contains("`r")){throw 'Provide a short absolute AndroidBuildRoot without quotes/newlines'}
 $androidOut=[IO.Path]::GetFullPath($AndroidBuildRoot).Replace('\','/')
 if($androidOut -eq $root -or $androidOut -eq $client){throw 'Use a dedicated Android build directory'}
 New-Item -ItemType Directory -Path $androidOut -Force | Out-Null
 $marker="$androidOut/mat-capabilities-workspace.txt"
 if(Test-Path -LiteralPath $marker){if((Get-Content -Raw -LiteralPath $marker).Trim() -ne $client){throw 'Android build directory belongs to another checkout'}}
 elseif(@(Get-ChildItem -LiteralPath $androidOut -Force).Count){throw 'First use requires an empty Android build directory'}
 [IO.File]::WriteAllText($marker,$client,[Text.UTF8Encoding]::new($false))
 $init=@"
gradle.beforeProject { p ->
    def outputRoot = new File('$androidOut')
    p.layout.buildDirectory.set(new File(outputRoot, p.name))
    ['com.android.application', 'com.android.library'].each { pluginId ->
        p.pluginManager.withPlugin(pluginId) {
            p.android.externalNativeBuild.cmake.buildStagingDirectory = new File(outputRoot, 'native-cmake/' + p.name)
            p.android.externalNativeBuild.ndkBuild.buildStagingDirectory = new File(outputRoot, 'native-ndk/' + p.name)
        }
    }
}
gradle.projectsEvaluated {
    def app = gradle.rootProject.project(':app')
    app.android.buildTypes.debug.applicationIdSuffix = '.vplus.matcapabilities'
    app.android.buildTypes.debug.resValue 'string', 'app_label', 'Moonlight MAT validation'
    app.android.buildTypes.debug.resValue 'string', 'app_label_root', 'Moonlight MAT validation'
}
"@
 [IO.File]::WriteAllText("$out/client.init.gradle",$init,[Text.UTF8Encoding]::new($false))
 Push-Location $client
 try {
  & ./gradlew.bat --offline --no-daemon --max-workers=4 --init-script "$out/client.init.gradle" "-PaudioHapticsSdkDir=$root/third-party/moonlight-audio-haptics" :app:assembleNonRootDebug *> "$out/full-client.log"
  if($LASTEXITCODE){throw 'Full client APK build failed'}
 } finally {Pop-Location}
 $apk="$androidOut/app/outputs/apk/nonRoot/debug/app-nonRoot-debug.apk"
 & "$SdkRoot/build-tools/36.0.0/apksigner.bat" verify --verbose $apk *> "$out/apk-signature.log"
 if($LASTEXITCODE){throw 'Full APK signature verification failed'}
}
$automatic=Get-Content -Raw "$out/negotiation/proof.json" | ConvertFrom-Json
$strict=Get-Content -Raw "$out/strict-negative/proof.json" | ConvertFrom-Json
if($automatic.result -ne 'PASS' -or $automatic.results.Count -ne 21 -or $strict.results.Count -ne 15){throw 'Incomplete RTSP coverage'}
$sourcePaths=@($PSCommandPath,"$PSScriptRoot/../mat-android-offline/verify.ps1")
foreach($name in @('negotiation_client.c','capability_negotiation_test.py','negotiation_test.py','session_jni.c')){
 $sourcePaths+="$PSScriptRoot/../mat-android-session/$name"
}
foreach($relative in @(git -C $client diff --name-only 6c81179991b46c16a01563b6011e79e4cb4239ac HEAD)){
 if(Test-Path -LiteralPath "$client/$relative" -PathType Leaf){$sourcePaths+="$client/$relative"}
}
foreach($relative in @(git -C $common diff --name-only 773bebded05624e275106ce1fa4bb9aa32726535 HEAD)){
 $sourcePaths+=@("$common/$relative","$clientCommon/$relative")
}
$sources=@($sourcePaths | ForEach-Object {@{path=(Resolve-Path -LiteralPath $_).Path;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLower()}})
$artifacts=@("$out/offline/proof.json","$out/negotiation/proof.json","$out/strict-negative/proof.json","$out/negotiation-client.exe")
if($apk){$artifacts+=@($apk,"$out/full-client.log","$out/apk-signature.log")}
$hashes=@($artifacts | ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLower()}})
@{result='PASS';scope='Pre-session MAT eligibility and actual RTSP negotiation; no physical HDMI or real-time capture acceptance';
 commonCommit=$sharedHead;clientCommit=(git -C $client rev-parse HEAD);hostCommit=(git -C $root rev-parse HEAD);
 commonCtestsPerCopy=14;javaJniPlayerGroups=12;outputPolicyCases=18;automaticRtspCases=21;strictNegativeRtspCases=15;
 fullClientApkBuilt=[bool]$BuildFullClient;androidDeviceTested=$false;realTimeCaptureTested=$false;atmosRecognitionVerified=$false;
 sourceHashes=$sources;artifactHashes=$hashes} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$out/proof.json" -Encoding utf8
Write-Host "PASS MAT capability selection: $out/proof.json"
