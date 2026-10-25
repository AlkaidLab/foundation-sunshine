param(
 [Parameter(Mandatory=$true)][string]$Sdk,
 [Parameter(Mandatory=$true)][string]$Ndk,
 [Parameter(Mandatory=$true)][string]$OpenSsl,
 [Parameter(Mandatory=$true)][string]$CommonBuild,
 [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Fresh output directory required'}
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$root=(Resolve-Path "$PSScriptRoot/../../..").Path.Replace('\','/')
$common="$root/third-party/moonlight-common-c"
$lib=(Resolve-Path -LiteralPath $CommonBuild).Path.Replace('\','/')
$crypto=(Resolve-Path -LiteralPath $OpenSsl).Path.Replace('\','/')
$cmake=(Resolve-Path "$Sdk/cmake/3.22.1/bin/cmake.exe").Path
& $cmake -S $PSScriptRoot -B "$out/android" -G Ninja "-DCMAKE_MAKE_PROGRAM=$Sdk/cmake/3.22.1/bin/ninja.exe" "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-23 -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DOPENSSL_ROOT=$crypto" *> "$out/android-config.log"
if($LASTEXITCODE){throw 'Android configuration failed'}
& $cmake --build "$out/android" --target mat-udp-probe mat-paced-sender --parallel 2 *> "$out/android-build.log"
if($LASTEXITCODE){throw 'Android probe build failed'}
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$includes=@("-I$common/src","-I$common/enet/include","-I$common/nanors","-I$common/nanors/deps","-I$common/nanors/deps/obl")
$libraries=@("$lib/libmoonlight-common-c.a","$lib/enet/libenet.a",'-lcrypto','-lws2_32','-lwinmm','-lbcrypt','-lcrypt32','-liphlpapi','-ladvapi32','-lshell32','-lole32','-luuid')
& g++.exe -O2 -std=c++23 -DNDEBUG -DHAS_SOCKLEN_T @includes "$PSScriptRoot/paced_sender.cpp" @libraries -o "$out/paced-sender.exe" *> "$out/windows-sender-build.log"
if($LASTEXITCODE){throw 'Windows sender build failed'}
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T @includes "$PSScriptRoot/udp_probe.c" @libraries -o "$out/windows-probe.exe" *> "$out/windows-probe-build.log"
if($LASTEXITCODE){throw 'Windows receiver build failed'}
Write-Output "Built software-only probes in $out; no emulator or native capture started."
