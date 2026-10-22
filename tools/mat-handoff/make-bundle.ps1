param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path
$out=[IO.Path]::GetFullPath($Output)
if((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath "$out.zip")){throw 'Use a new bundle directory and ZIP name'}
$fixture='spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw'
$expected='79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6'
if((Get-FileHash -LiteralPath "$root/$fixture").Hash.ToLower() -ne $expected){throw 'Frozen fixture mismatch'}
# Strict allowlist: no recursive run-directory copy, certificates, private keys,
# state, credentials, binaries, signing keystores or full object captures.
$files=@('docs/ATMOS-MAT-HANDOFF.md',$fixture,
 'spatial-object-spike/mat-android-session/C-PROOF.json',
 'spatial-object-spike/mat-android-session/session-final/proof.json',
 'spatial-object-spike/mat-android-session/dump-session-final/proof.json',
 'spatial-object-spike/mat-android-session/negative03/proof.json',
 'spatial-object-spike/mat-android-session/regression-final/proof.json',
 'spatial-object-spike/mat-android-session/MAT-ANDROID-C-RESULT.txt',
 'spatial-object-spike/mat-android-offline/MAT-ANDROID-OFFLINE-RESULT.txt')
foreach($file in $files){if(!(Test-Path -LiteralPath "$root/$file" -PathType Leaf)){throw "Missing handoff input: $file"}}
New-Item -ItemType Directory -Path $out | Out-Null
$manifest=@(foreach($file in $files){
 $target=Join-Path $out $file
 New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
 Copy-Item -LiteralPath "$root/$file" -Destination $target
 [ordered]@{path=$file;bytes=(Get-Item -LiteralPath $target).Length;sha256=(Get-FileHash -LiteralPath $target).Hash.ToLower()}
})
$repos=[ordered]@{}
foreach($name in @('host','client','common')){
 $path=switch($name){host{$root} client{"$root/spatial-object-spike/moonlight-vplus"} common{"$root/third-party/moonlight-common-c"}}
 $commit=git -C $path rev-parse HEAD
 if($LASTEXITCODE){throw "Cannot identify $name commit"}
 $repos[$name]=$commit
}
[ordered]@{created=(Get-Date).ToString('o');scope='Private local transfer only: finite fixture and historical evidence; no toolchains or binaries. Read docs/ATMOS-MAT-HANDOFF.md. Do not publish game recording without permission.';repositories=$repos;files=$manifest} |
 ConvertTo-Json -Depth 6 | Out-File -LiteralPath "$out/BUNDLE-MANIFEST.json" -Encoding utf8
Compress-Archive -LiteralPath "$out/docs","$out/spatial-object-spike","$out/BUNDLE-MANIFEST.json" -DestinationPath "$out.zip" -CompressionLevel Optimal
$zipHash=(Get-FileHash -LiteralPath "$out.zip").Hash.ToLower()
Write-Host "BUNDLE $out.zip"
Write-Host "SHA256 $zipHash"
