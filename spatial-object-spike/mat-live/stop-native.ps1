param([Parameter(Mandatory=$true)][string]$Metadata)
$ErrorActionPreference='Stop'
$state=Get-Content -Raw -LiteralPath $Metadata | ConvertFrom-Json
$process=Get-Process -Id $state.publisherPid -ErrorAction Stop
if($process.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $state.publisherCreated -or [IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($state.producer)){
 throw 'Publisher identity changed; refusing to signal another capture'
}
$signal=[Threading.EventWaitHandle]::OpenExisting($state.stopEvent)
try{[void]$signal.Set()}finally{$signal.Dispose()}
Write-Host 'Explicit stop signalled; inspect capture.log for complete-cycle stop and original-byte restoration.'
