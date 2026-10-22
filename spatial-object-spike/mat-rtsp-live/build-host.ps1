$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
# Existing local C++/WinRT v2.0.250303.1 exposes GraphicsCaptureSession5.
# This directory has WinRT projections, not replacement C/C++ system headers.
$env:CPATH="$root/build/dlssnr-toolchain/ucrt64/include"
$rcTools="$root/build/mat-rtsp-live/rc-tools"
New-Item -ItemType Directory -Force -Path $rcTools | Out-Null
& gcc.exe -O2 -municode "$PSScriptRoot/windres_paths.c" -static -lshlwapi -o "$rcTools/windres.exe"
if($LASTEXITCODE){throw 'RC path adapter build failed'}
$env:PATH="$rcTools;"+$env:PATH
$env:PKG_CONFIG_PATH="$root/build/miniupnp-current/install/lib/pkgconfig;C:/msys64/ucrt64/lib/pkgconfig"
$expected=@{
 # Follow the committed gitlink, not the pre-MAT baseline. Never use --remote
 # here: the two parent PRs must consume the same reviewed common-c commit.
 'third-party/moonlight-common-c'=(git -C $root rev-parse 'HEAD:third-party/moonlight-common-c')
 'third-party/pyrowave'=(git -C $root rev-parse 'HEAD:third-party/pyrowave')
}
foreach($dependency in $expected.Keys){
 $actual=git -C "$root/$dependency" rev-parse HEAD
 if($actual -ne $expected[$dependency]){throw "Dependency not aligned: $dependency $actual"}
}
$graniteExpected=git -C "$root/third-party/pyrowave" rev-parse 'HEAD:third_party/Granite'
$graniteActual=git -C "$root/third-party/pyrowave/third_party/Granite" rev-parse HEAD
if($graniteExpected -ne $graniteActual){throw 'Granite does not match the committed PyroWave gitlink'}

# A normal Sunshine executable: no diagnostic transport macros, mocks, forced
# headers or decoder dependencies. Only optional GUI/tray/enhancements disabled.
# Boost's unused Windows Event Log RC has a MinGW path-with-spaces problem.
& cmake.exe -S $root -B "$root/build/mat-rtsp-live" -G Ninja `
 -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
 -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release `
 "-DCMAKE_RC_COMPILER=$rcTools/windres.exe" `
 -DBUILD_TESTS=OFF -DBUILD_WEB_UI=OFF -DBUILD_DOCS=OFF -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_REQUIRE_TRAY=OFF `
 -DSUNSHINE_DLSSNR=OFF -DSUNSHINE_RTX_HDR=OFF -DFETCH_DRIVER_DEPS=OFF -DDRIVER_DEPS_REQUIRED=OFF `
 -DCMAKE_RC_FLAGS= -DBOOST_LOG_WITHOUT_EVENT_LOG=ON `
 "-DFETCHCONTENT_SOURCE_DIR_BOOST=$root/build/codex/_deps/boost-src" `
 "-DGUI_DIR=$root/build/dlssnr-paired-91080d54-074/portable/Sunshine/assets/gui" `
 "-DCMAKE_EXE_LINKER_FLAGS=-B`"$root/build/dlssnr-toolchain/ucrt64/bin/`" -B`"$root/build/dlssnr-toolchain/ucrt64/lib/`""
if($LASTEXITCODE){throw 'Host configure failed'}
$cache=Get-Content -Raw "$root/build/mat-rtsp-live/CMakeCache.txt"
foreach($setting in @('FETCH_DRIVER_DEPS:BOOL=OFF','BUILD_TESTS:BOOL=OFF','SUNSHINE_ENABLE_TRAY:BOOL=OFF')){
 if(!$cache.Contains($setting)){throw "CMake reset the explicit test profile; reconfigure before building: $setting"}
}
& cmake.exe --build "$root/build/mat-rtsp-live" --target sunshine --parallel 4
if($LASTEXITCODE){throw 'Host build failed'}
