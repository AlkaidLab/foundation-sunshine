param([string]$Output="$PSScriptRoot/client-build")
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$out=[IO.Path]::GetFullPath($Output).Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$common="$root/third-party/moonlight-common-c"
& cmake.exe -S $common -B "$out/common" -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64
if($LASTEXITCODE){throw 'common configure failed'}
& cmake.exe --build "$out/common" --parallel 2
if($LASTEXITCODE){throw 'common build failed'}
& gcc.exe -O2 -DNDEBUG -DHAS_SOCKLEN_T "-I$common/src" "-I$common/enet/include" "$PSScriptRoot/session_client.c" "$PSScriptRoot/../mat-diagnostics/MatDumpFile.c" "$out/common/libmoonlight-common-c.a" "$out/common/enet/libenet.a" -static -lcrypto -lws2_32 -lwinmm -lbcrypt -lcrypt32 -liphlpapi -o "$out/mat-session-client.exe"
if($LASTEXITCODE){throw 'full session client build failed'}
