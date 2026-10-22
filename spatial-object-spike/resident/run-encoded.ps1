param([ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$RunName='encoded01',[ValidateRange(1,30)][int]$Seconds=10,[switch]$ControlledSource)
$ErrorActionPreference='Stop'
if(Get-Process collector,encoded_probe -ErrorAction SilentlyContinue){throw 'Stop the existing collector first'}
$out=Join-Path $PSScriptRoot $RunName
if(Test-Path -LiteralPath $out){throw 'Output already exists'}
$source=Join-Path $PSScriptRoot 'encoded_tap.dll';$deployed='C:\ProgramData\SpatialObjectSpike\engine_tap_encoded_v1.dll'
if(-not (Test-Path -LiteralPath $deployed)){Copy-Item -LiteralPath $source -Destination $deployed}
if((Get-FileHash $source).Hash -ne (Get-FileHash $deployed).Hash){throw 'Pinned DLL hash mismatch; do not overwrite'}
$child=Start-Process -FilePath (Join-Path $PSScriptRoot 'encoded_probe.exe') -ArgumentList @(('"'+$out+'"'),"$Seconds") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out.log" -RedirectStandardError "$out.err"
$retained=$child.Handle
try {
    Start-Sleep -Milliseconds 400
    if($ControlledSource) {
        $emitter=Join-Path (Split-Path -Parent $PSScriptRoot) 'spatial_probe.exe'
        $sourceProcess=Start-Process -FilePath $emitter -ArgumentList '--emit --dev 1 --seconds 5 --objects 2 --variant 0' -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out-source.log" -RedirectStandardError "$out-source.err"
        $sourceHandle=$sourceProcess.Handle
    }
    if(Test-Path -LiteralPath $out) {
        & (Join-Path $PSScriptRoot 'session_audit.exe') | Out-File (Join-Path $out 'sessions-before.log') -Encoding utf8
        Get-Process Nioh3 -ErrorAction SilentlyContinue | ForEach-Object {"GamePID=$($_.Id) CaptureDllLoaded=$([bool]($_.Modules.ModuleName -match 'engine_tap|spatial_tap'))"} | Out-File (Join-Path $out 'game-before.log')
    }
    $child.WaitForExit()
    if($ControlledSource){$sourceProcess.WaitForExit();"SourcePID=$($sourceProcess.Id) Exit=$($sourceProcess.ExitCode)" | Out-File "$out-source.exit"}
    if(Test-Path -LiteralPath $out) {
        & (Join-Path $PSScriptRoot 'session_audit.exe') | Out-File (Join-Path $out 'sessions-after.log') -Encoding utf8
        Get-Process Nioh3 -ErrorAction SilentlyContinue | ForEach-Object {"GamePID=$($_.Id) CaptureDllLoaded=$([bool]($_.Modules.ModuleName -match 'engine_tap|spatial_tap'))"} | Out-File (Join-Path $out 'game-after.log')
    }
    "Exit=$($child.ExitCode)" | Out-File "$out.exit"
} finally {
    if(-not $child.HasExited){New-Item -ItemType File -Path (Join-Path $out 'STOP') -Force | Out-Null;$child.WaitForExit(10000) | Out-Null}
}
