param([Parameter(Mandatory=$true)][string]$CommonBuild,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Output){throw 'Use a fresh live-check build directory'}
$root=(Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$common="$root/third-party/moonlight-common-c";$lib=(Resolve-Path -LiteralPath $CommonBuild).Path.Replace('\','/')
$out=(New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
$includes=@("-I$common/src","-I$common/enet/include","-I$common/nanors","-I$common/nanors/deps","-I$common/nanors/deps/obl")
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T @includes -c "$PSScriptRoot/live_check_transport.c" -o "$out/transport.o"
if($LASTEXITCODE){throw 'Production client adapter build failed'}
$libraries=@("$out/transport.o","$lib/libmoonlight-common-c.a","$lib/enet/libenet.a",'-lcrypto','-lws2_32','-lwinmm','-lbcrypt','-lcrypt32','-liphlpapi','-ladvapi32','-lshell32','-lole32','-luuid')
& g++.exe -O2 -std=c++23 -DNDEBUG -DHAS_SOCKLEN_T @includes "$PSScriptRoot/live_check.cpp" @libraries -o "$out/mat-live-check.exe"
if($LASTEXITCODE){throw 'Native-only loopback checker build failed'}
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T @includes "$root/tests/mat-live/live_check_transport_test.c" @libraries -o "$out/transport-test.exe"
if($LASTEXITCODE){throw 'Generated software transport test build failed'}
& "$out/transport-test.exe" *> "$out/transport-test.log"
if($LASTEXITCODE){throw 'Software UDP/RTP/FEC/authenticated receive test failed'}
Get-Content "$out/transport-test.log"
& gcc.exe -O2 -std=c11 -DNDEBUG -DHAS_SOCKLEN_T -DMOCK_USE_UDP @includes "$root/tests/mat-live/mock_output_test.c" @libraries -o "$out/mock-output-udp-test.exe"
if($LASTEXITCODE){throw 'Mock output UDP integration build failed'}
& "$out/mock-output-udp-test.exe" *> "$out/mock-output-udp-test.log"
if($LASTEXITCODE){throw 'Mock output UDP integration failed'}
Get-Content "$out/mock-output-udp-test.log"
Get-FileHash "$out/mat-live-check.exe","$out/transport-test.exe"
