param(
 [Parameter(Mandatory=$true)][string]$OfflineBuild,
 [Parameter(Mandatory=$true)][string]$Serial,
 [Parameter(Mandatory=$true)][string]$Output,
 [switch]$PlayResearchPcm,
 [switch]$SkipInstall
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'New device evidence directory required'}
$build=(Resolve-Path -LiteralPath $OfflineBuild).Path
$apk=Join-Path $build 'mat-offline-diagnostic.apk'
$offline=Get-Content -Raw -LiteralPath (Join-Path $build 'proof.json') | ConvertFrom-Json
$apkHash=(Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLower()
$fingerprint=@($offline.files | Where-Object { [IO.Path]::GetFileName($_.path) -eq 'mat-offline-diagnostic.apk' })
if($offline.result -ne 'PASS' -or $fingerprint.Count -ne 1 -or $fingerprint[0].sha256 -ne $apkHash){throw 'APK does not match passing offline evidence'}
$out=(New-Item -ItemType Directory -Path $Output).FullName
if(!$SkipInstall){
 & adb -s $Serial install --no-incremental --no-streaming -r $apk 2>&1 | Tee-Object -FilePath "$out/install.log"
 if($LASTEXITCODE){throw 'Diagnostic APK install failed'}
} else {
 'Installation completed separately; checking installed APK hash before instrumentation.' | Set-Content -LiteralPath "$out/install.log"
}
$installed=@(& adb -s $Serial shell pm path com.limelight.matdiagnostic)
if($LASTEXITCODE -or $installed.Count -ne 1 -or "$($installed[0])" -notmatch '^package:(.+)$'){throw 'Cannot identify one installed diagnostic APK'}
$installedPath=$Matches[1].Trim()
$installedDigest=& adb -s $Serial shell sha256sum $installedPath
if($LASTEXITCODE -or "$installedDigest" -notmatch '^([0-9a-fA-F]{64})\s' -or $Matches[1].ToLower() -ne $apkHash){throw 'Installed APK does not match offline evidence'}
$instrumentArgs=@('-w')
if($PlayResearchPcm){$instrumentArgs+=@('-e','playResearchPcm','true')}
$lines=@(& adb -s $Serial shell am instrument @instrumentArgs com.limelight.matdiagnostic/com.limelight.binding.audio.MatDeviceCheck 2>&1)
$lines | Tee-Object -FilePath "$out/instrumentation.log"
if($LASTEXITCODE){throw 'Device instrumentation command failed'}
$fields=[ordered]@{}
foreach($line in $lines){
 if("$line" -match '^INSTRUMENTATION_RESULT: ([^=]+)=(.*)$'){$fields[$Matches[1]]=$Matches[2]}
}
if($fields['result'] -ne 'PASS' -or !($lines -match '^INSTRUMENTATION_CODE: -1$')){throw 'Device checks failed; inspect instrumentation.log'}
@{result='PASS';time=[DateTimeOffset]::Now.ToString('o');apkSha256=$apkHash;
 offlineProofSha256=(Get-FileHash -LiteralPath "$build/proof.json" -Algorithm SHA256).Hash.ToLower();
 runnerSha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLower();installedApkHashVerified=$true;
 device=$fields;scope='Device ART/JNI/player and platform capability checks; optional fixed-fixture PCM audition';
 networkSessionTested=$false;speakerPcmSubmitted=[bool]$PlayResearchPcm;acousticOutputConfirmed=$false;atmosRecognitionTested=$false} |
 ConvertTo-Json -Depth 5 | Out-File -LiteralPath "$out/proof.json" -Encoding utf8
Write-Host "PASS device diagnostic: $out/proof.json"
