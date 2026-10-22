param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
# Run in PowerShell 7; old RTP script defaults are preserved for E-AC3 tests.
& "$PSScriptRoot/../rtp-spike/build.ps1" -Mat -Output $Output
