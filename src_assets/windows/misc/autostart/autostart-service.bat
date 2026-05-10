@echo off
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"

rem Set the service to auto-start
sc config SunshineService start= auto
