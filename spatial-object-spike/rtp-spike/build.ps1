param(
  [string]$ExistingBuild = "$PSScriptRoot/../../build/dlssnr-host",
  [string]$Output = "$PSScriptRoot/build07",
  [switch]$Mat
)
$ErrorActionPreference = 'Stop'
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
$root = (Resolve-Path "$PSScriptRoot/../..").Path.Replace('\','/')
$existing = (Resolve-Path $ExistingBuild).Path
if (Test-Path -LiteralPath $Output) { throw 'Build output must be new' }
$out = (New-Item -ItemType Directory -Path $Output).FullName.Replace('\','/')
$commands = Get-Content -Raw -LiteralPath "$existing/compile_commands.json" | ConvertFrom-Json
$utf8 = [Text.UTF8Encoding]::new($false)
function Slice-Source([string]$Text,[string]$Begin,[string]$End) {
  $a=$Text.IndexOf($Begin,[StringComparison]::Ordinal)
  if ($a -lt 0) { throw "Source start marker missing: $Begin" }
  $b=$Text.IndexOf($End,$a+$Begin.Length,[StringComparison]::Ordinal)
  if ($b -lt 0) { throw "Source end marker missing: $End" }
  return $Text.Substring($a,$b-$a)
}
function Read-Source([string]$Path) { return (Get-Content -Raw -LiteralPath $Path).Replace("`r`n","`n") }

# Unrelated video dependencies in this checkout are incomplete. Mechanically
# extract the ACTUAL audio and Windows send functions VERBATIM, not rewritten.
# Only the state/fixture frontend is isolated; no RTSP/application startup.
$stream = Read-Source "$root/src/stream.cpp"
$windows = Read-Source "$root/src/platform/windows/misc.cpp"
$chunks = [ordered]@{}
$chunks.payload = Slice-Source $stream '  namespace audio_payload {' '  using av_session_id_t'
$chunks.encode = Slice-Source $stream "  static inline int`n  encode_audio(" "  static inline void`n  while_starting_do_nothing"
$chunks.broadcast = Slice-Source $stream "  void`n  audioBroadcastThread(" "`n#ifdef SUNSHINE_AUDIO_RTP_DIAGNOSTIC`n  #include `"stream_audio_replay.inc`""
$chunks.address = Slice-Source $windows "  SOCKADDR_IN`n  to_sockaddr(" '  // Use UDP segmentation offload'
$chunks.priority = Slice-Source $windows "  void`n  adjust_thread_priority(" "  void`n  streaming_will_start()"
$chunks.send = Slice-Source $windows "  bool`n  send(send_info_t &send_info)" '  class qos_t'
$matIncludes = if($Mat) { "#include <iostream>`n#include `"$root/src/spatial_transport/mat_session_source.h`"`n#include `"$root/src/spatial_transport/mat_replay_state.h`"`n#include `"$root/src/spatial_audio/mat_replay_source.h`"`n" } else { '' }
$matEntry = if($Mat) { "`n#include `"$root/src/stream_mat_replay.inc`"`n" } else { '' }
$sender = $matIncludes + (Read-Source "$PSScriptRoot/sender_prelude.h") + "`n" + $chunks.payload + $chunks.encode + $chunks.broadcast + "`n#include `"$root/src/stream_audio_replay.inc`"`n" + $matEntry + "}`n"
$platform = (Read-Source "$PSScriptRoot/platform_prelude.h") + "`n" + $chunks.address + $chunks.priority + $chunks.send + "`n}`n"
[IO.File]::WriteAllText("$out/sender.generated.cpp",$sender,$utf8)
[IO.File]::WriteAllText("$out/platform.generated.cpp",$platform,$utf8)
$proof = [ordered]@{}
foreach ($name in $chunks.Keys) {
  $proof[$name] = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes($chunks[$name])))
}
[IO.File]::WriteAllText("$out/source-chunks.json",($proof|ConvertTo-Json),$utf8)

Push-Location $existing
try {
  $main = if($Mat) { "$PSScriptRoot/../mat-rtp-spike/main.cpp" } else { "$PSScriptRoot/probe_main.cpp" }
  $units = [ordered]@{ main=$main; sender="$out/sender.generated.cpp"; platform="$out/platform.generated.cpp"; crypto="$root/src/crypto.cpp" }
  foreach ($name in $units.Keys) {
    $original = if ($name -eq 'crypto') { 'crypto' } else { 'main' }
    $entry = $commands | Where-Object { $_.file.Replace('\','/') -eq "$root/src/$original.cpp" -and $_.command -match 'sunshine.dir' } | Select-Object -First 1
    if (!$entry) { throw "Missing compiler command for $original" }
    $command = $entry.command -replace ' -o \S+ ', " -o `"$out/$name.obj`" "
    $command = $command -replace ' -c .*$', " -c `"$($units[$name])`""
    $command = $command -replace ' -O3 ', ' -O1 '
    $command = $command -replace '\.exe ', '.exe -DSUNSHINE_AUDIO_RTP_DIAGNOSTIC '
    if($Mat) { $command = $command -replace '\.exe ', '.exe -DSUNSHINE_MAT_RTP_DIAGNOSTIC ' }
    $argumentsText = $command -replace '^.*?g\+\+\.exe\s+', ''
    [IO.File]::WriteAllText("$out/$name.rsp",$argumentsText,$utf8)
    Write-Host "Compiling $name (production code / state-only diagnostic frontend)"
    & 'C:/msys64/ucrt64/bin/g++.exe' "@$out/$name.rsp"
    if ($LASTEXITCODE) { throw "Compile $name failed: $LASTEXITCODE" }
  }
  $ninja=Get-Content -LiteralPath "$existing/build.ninja"
  $line=$ninja|Where-Object { $_ -match '^build sunshine.exe:' }|Select-Object -First 1
  $index=[Array]::IndexOf($ninja,$line); $props=@{}
  for ($i=$index+1; $i -lt $ninja.Count -and $ninja[$i] -match '^  (\w+) = (.*)$'; $i++) { $props[$Matches[1]]=$Matches[2] }
  $objects=($units.Keys|ForEach-Object { '"' + "$out/$_.obj" + '"' }) -join ' '
  $objects+=' CMakeFiles/sunshine.dir/third-party/nanors/rs.c.obj CMakeFiles/sunshine.dir/third-party/nanors/deps/obl/oblas_common.c.obj CMakeFiles/sunshine.dir/third-party/nanors/deps/obl/oblas_lite.c.obj'
  [IO.File]::WriteAllText("$out/link.rsp","$objects $($props.LINK_PATH) $($props.LINK_LIBRARIES)",$utf8)
  & cmd.exe /d /c "C:\msys64\ucrt64\bin\g++.exe $($props.FLAGS) $($props.LINK_FLAGS) @`"$out/link.rsp`" -o `"$out/sunshine-audio-replay.exe`""
  if ($LASTEXITCODE) { throw "Link failed: $LASTEXITCODE" }
  Copy-Item -LiteralPath 'C:/msys64/ucrt64/bin/libiconv-2.dll' -Destination $out
} finally { Pop-Location }

$common="$root/third-party/moonlight-common-c"
$receiver = if($Mat) { "$PSScriptRoot/../mat-rtp-spike/receiver.c" } else { "$PSScriptRoot/receiver.c" }
$arguments=@('-std=c11','-O2','-DNDEBUG','-DHAS_SOCKLEN_T',"-I$common/src","-I$common/enet/include","-I$common/nanors","-I$common/nanors/deps","-I$common/nanors/deps/obl",
  $receiver,"$common/src/RtpAudioQueue.c","$common/src/PlatformCrypto.c",
  "$common/nanors/rs.c","$common/nanors/deps/obl/oblas_common.c","$common/nanors/deps/obl/oblas_lite.c",
  '-static','-lcrypto','-lws2_32','-lbcrypt','-lcrypt32','-o',"$out/moonlight-audio-dump.exe")
if($Mat) { $arguments += @("-I$root/src/spatial_transport","$common/src/MatDepacketizer.c") }
Write-Host 'Compiling unmodified Moonlight RTP queue / crypto + dump frontend'
& 'C:/msys64/ucrt64/bin/gcc.exe' @arguments
if ($LASTEXITCODE) { throw "Receiver build failed: $LASTEXITCODE" }
if($Mat) {
  & 'C:/msys64/ucrt64/bin/gcc.exe' -std=c11 -O2 -Wall -Wextra "-I$common/src" "-I$root/src/spatial_transport" "$PSScriptRoot/../mat-rtp-spike/unit.c" "$common/src/MatDepacketizer.c" -static -o "$out/mat-wire-unit.exe"
  if($LASTEXITCODE){throw 'MAT unit build failed'}
  $manifest=[ordered]@{}
  foreach($path in @('src/stream.cpp','src/stream.h','src/crypto.cpp','src/thread_safe.h','src/stream_mat_replay.inc','src/spatial_audio/mat_replay_source.h','src/spatial_audio/encoded_audio_source.h','src/spatial_transport/mat_wire.h','src/spatial_transport/mat_fragmenter.h','src/spatial_transport/mat_replay_state.h','src/platform/windows/misc.cpp','third-party/moonlight-common-c/src/RtpAudioQueue.c','third-party/moonlight-common-c/src/PlatformCrypto.c','third-party/moonlight-common-c/src/MatWire.h','third-party/moonlight-common-c/src/MatDepacketizer.c','third-party/moonlight-common-c/src/MatDepacketizer.h','spatial-object-spike/mat-rtp-spike/receiver.c','spatial-object-spike/mat-rtp-spike/main.cpp','spatial-object-spike/mat-rtp-spike/unit.c','spatial-object-spike/mat-rtp-spike/verify.py','spatial-object-spike/mat-rtp-spike/regression.py','spatial-object-spike/mat-rtp-spike/build.ps1','spatial-object-spike/rtp-spike/sender_prelude.h','spatial-object-spike/rtp-spike/platform_prelude.h','spatial-object-spike/rtp-spike/build.ps1')) {
    $manifest[$path]=(Get-FileHash -LiteralPath "$root/$path").Hash
  }
  [IO.File]::WriteAllText("$out/manifest.json",($manifest|ConvertTo-Json),$utf8)
}
Write-Host "Ready: $out"
