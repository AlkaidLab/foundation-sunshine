@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
rundll32.exe powrprof.dll,SetSuspendState 0,1,0