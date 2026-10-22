param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
if(Test-Path -LiteralPath $Output){throw 'New build directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
foreach($variant in @('host-common','android-common')) {
 $source=if($variant -eq 'host-common'){"$root/third-party/moonlight-common-c"}else{"$root/spatial-object-spike/moonlight-vplus/app/src/main/jni/moonlight-core/moonlight-common-c"}
 & cmake.exe -S $source -B "$out/$variant" -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64
 if($LASTEXITCODE){throw "$variant configure failed"}
 & cmake.exe --build "$out/$variant" --parallel 4
 if($LASTEXITCODE){throw "$variant build failed"}
}
$existing="$root/build/dlssnr-host";$commands=Get-Content -Raw "$existing/compile_commands.json"|ConvertFrom-Json
Push-Location $existing
try {
 foreach($unit in @('rtsp','stream','config','audio')) {
  $entry=$commands|Where-Object {$_.file.Replace('\','/') -eq "$root/src/$unit.cpp" -and $_.command -match 'sunshine.dir'}|Select-Object -First 1
  if(!$entry){throw "Missing compile command $unit"}
  $command=$entry.command -replace ' -o \S+ '," -o `"$out/$unit.obj`" "
  $command=$command -replace ' -O3 ',' -O1 '
  # common-c is aligned to b91c7cd, matching the Android checkout. Compile
  # against the real host include chain; no forced compatibility header.
  $argsText=$command -replace '^.*?g\+\+\.exe\s+',''
  [IO.File]::WriteAllText("$out/$unit.rsp",$argsText,[Text.UTF8Encoding]::new($false))
  & g++.exe "@$out/$unit.rsp"
  if($LASTEXITCODE){throw "Host production object $unit failed"}
 }
}finally{Pop-Location}
Write-Host "READY $out"
