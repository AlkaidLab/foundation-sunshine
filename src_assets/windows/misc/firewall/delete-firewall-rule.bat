@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
set "PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"

set RULE_NAME=Sunshine

rem Delete the rule
"%SystemRoot%\System32\netsh.exe" advfirewall firewall delete rule name=%RULE_NAME%
