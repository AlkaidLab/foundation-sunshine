@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"

set RULE_NAME=Sunshine

rem Delete the rule
netsh advfirewall firewall delete rule name=%RULE_NAME%
